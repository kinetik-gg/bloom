# Interaction Model

Status: accepted

Updated: 2026-09-14

## Purpose

This document owns Bloom's keyboard and pointer bindings. It is the single list: a binding that is not
here is not a binding, and a key that appears here appears exactly once per editor.

Bloom is Adobe-first. An artist arriving from After Effects or Photoshop should find the navigation and
editing keys where they already are, and Bloom does not spend a bare letter key on something those
hosts reach with a modifier — bare letters belong to tools.

## Rules

- **One meaning per key per editor.** A key does not change meaning depending on what is selected.
- **A modifier-free letter is a tool, not a command**, except Timeline B/N range marks. Commands that destroy, duplicate, or toggle
  state take a modifier or live in a menu.
- **A command may live in a menu without owning a key.** Mute, collapse, and dissolve are exactly
  that: they are in the node context menu and bind nothing.
- **A focused text field keeps its own keys.** While an in-node value field or rename field holds
  focus, the canvas claims nothing — including Tab, which commits and travels. The one pair the
  canvas takes back is `Delete`/`Backspace`, and only where no text editor is actually OPEN in that
  field: a value field with its editor closed, a colour chip, a switch and a dropdown have no use for
  them, and swallowing them there made "click a card, press Delete" do nothing at all. A field whose
  text editor is open — the rename field, a text content row, a value field the artist has clicked
  into — keeps them.
- **Reserved keys are left unbound**, so the gesture that will own them is not taken first. Nothing
  is reserved at the moment: `Ctrl+G` and `Ctrl+Shift+G` were, and the Nodes canvas now binds them
  to grouping and ungrouping.

## Global

| Binding | Action |
| --- | --- |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / Redo |
| `Ctrl+N` | New project |
| `Ctrl+O` | Open project |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save As |
| `Ctrl+Q` | Quit |
| `Space` | Play/pause immediately from cache in any panel; focused text entry keeps Space |
| `Ctrl+Shift+Space` | RAM Preview: cache this composition's range, then play it |
| `Esc` | Cancel the RAM preview being cached (bound only while one is) |
| `F11` | Full screen |
| `` ` `` | Maximize or restore the panel under the pointer |
| `Alt` (held) | Reveal menu mnemonics |

## Viewer

Selected layers show their delivered-frame bounds polygon and anchor. Position moves the anchor in
composition space; anchor is an offset from local content centre, so changing it with position held
fixed moves content. The overlay follows zoom/pan and shows the actual delivered frame during a stale
preview. Cached display frames retain evaluated geometry without their Float32 process images, so
playback also shows the overlay without re-rendering. Empty content or another composition has no overlay. Selecting node cards can show multiple layer boundaries. Existing drag bindings are unchanged.

| Binding | Action |
| --- | --- |
| Composition selector | Switches the shared session to the chosen document composition and clears selection/time-local state as usual |
| Object selector | Selects the chosen current-composition layer; `None` clears selection |
| View → Fit / Actual Size / Zoom In / Zoom Out | Uses the viewer transform and shows the existing shortcut beside each item |
| View → Safe Areas / Centre Cross / Thirds / Rulers / Pixel Grid | Toggles display-only guides; Safe Areas uses the active composition's stored percentages |
| View → Safe Area Preset | Applies Broadcast, HD, Cinema, Social, or validated Custom percentages in one undoable composition edit |
| Select → All / None / Invert | Selects, clears, or inverts the current composition's layer boundaries |
| Composition menu / selector trailing menu | New, Duplicate, Rename, and Delete Composition; Delete refuses the last composition |
| `Ctrl+0` | Fit |
| `Ctrl+1` | Actual size (100%) |
| Middle drag | Pan |
| Wheel | Zoom about the pointer |
| `Esc` | Cancel the drag in flight |
| `Left` / `Right` | Step one frame |
| `Home` / `End` | Go to start / end |

The transport lives in the Viewer footer: go to start, step back, play/pause, step forward, go to
end, a loop toggle, and RAM Preview. **The keys are unchanged.** `Space`, `Ctrl+Shift+Space`, `Esc`,
`Left`, `Right`, `Home` and `End` all do exactly what they did when the transport sat in the
Timeline, and each still has one owner: `Space` and RAM Preview's pair are window-wide commands,
and the four frame-stepping actions are `Qt::WindowShortcut` actions owned by the Viewer.

Those four stand down while focus rests on a widget that consumes the same keys for its own
navigation -- the Timeline's layer stack does, and marks itself so. Widget focus wins; the step
action fires otherwise. A disabled action never claims the key, so the focused widget's own
navigation runs unchanged, and the footer's step buttons are disabled in lockstep so no button is
ever clickable while it would silently do nothing.

Loop is a real toggle, not a status glyph. With looping on, playback wraps to the start of the work
area, which is the only behaviour Bloom has ever had; with it off, playback lands on the last frame
of the range and pauses there. The choice persists in `playback/loop`.

RAM Preview stays its own command and its own button: caching a range and starting playback are
different things. The play button reports whether the range it is about to play is already cached.

| Gesture or control | Action |
| --- | --- |
| Click the frame/time readout | Opens an inline editor seeded with the current frame index |
| Type a frame number, `Return` | Seeks to that frame's exact mapped time, clamped to the composition's range |
| `Escape` or focus loss while editing | Cancels; an unparseable entry reverts in silence |
| Right-click the readout > Frames / Timecode | Switches the reading between a frame index and non-drop timecode |

The readout's editor always takes a frame INDEX, even while the label is showing timecode: it is
the one entry form that needs no parsing rules of its own. Frames / Timecode writes the same
`timeline/time-format` preference the Timeline's own View menu writes, so the ruler's tick labels
and this readout cannot disagree about the format. Exact composition seconds stay visible beside
either reading.

The guide toggles use `Ctrl+Alt+S` for Safe Areas and `Ctrl+Shift+C`, `Ctrl+Shift+T`,
`Ctrl+Shift+R`, and `Ctrl+Shift+P` for Centre Cross, Thirds, Rulers, and Pixel Grid. A pixel grid
is suppressed below 400% even when enabled. Guides follow the viewer's active zoom/pan transform;
they are not part of the display buffer or any export.

## Assets

Assets is a project-level composition browser. It has no modifier-free keyboard bindings: text
entry stays in the search or inline rename field, while persistent commands remain visible in the
panel menus or the composition context menu.

| Gesture or control | Action |
| --- | --- |
| Search field text | Filters the tree by composition name, case-insensitively |
| View > Expand All / Collapse All | Expands or collapses the asset tree |
| Add > New Composition… | Opens the name, size, frame-rate, and duration dialog, then adds one composition through the command path |
| Add > New Folder | Disabled; tooltip: `Folders arrive with asset organisation` |
| Select > All / None | Selects all visible rows or clears the selection |
| Double-click a composition | Switches the shared `CompositionSession` to that composition |
| Right-click a composition | Open, Rename (inline), Duplicate, or Delete |
| Footer: New Composition | Opens the same new-composition dialog |
| Footer: New Folder / Import | Disabled; Import tooltip: `Image and sequence import arrives with the media pipeline` |
| Footer: Delete | Deletes the active composition through one undoable command; the last composition is protected |

## Nodes

| Binding | Action |
| --- | --- |
| `Ctrl+0` | Fit the graph |
| `Ctrl+1` | Actual size (100%) |
| `Home` | Fit the graph (alias) |
| Middle drag | Pan |
| Wheel | Zoom about the pointer |
| `Tab` | Open the Add search at the pointer |
| `Ctrl+A` | Select all nodes |
| `Delete` / `Backspace` | Remove the selection |
| `Ctrl+D` | Duplicate the selection and start placing it |
| `Ctrl+G` | Group the selection in a node group |
| `Ctrl+Shift+G` | Ungroup every node group the selection is in |
| `Ctrl+Shift+A` | Arrange all nodes left to right, then fit the graph |
| `Ctrl+Alt+A` | Arrange the selected induced subgraph in place |
| `Enter` | Rename the selected layer node |
| Double-click a layer node | Rename it |
| Double-click a group frame | Rename the group |
| `Esc` | Cancel the gesture in flight |
| Left drag on a card | Move the selection |
| Left drag on a group frame | Move every member of that group |
| Left drag on a card's right edge | Resize the card |
| Left drag from a socket | Connect, rewire, or disconnect |
| `Ctrl` + right drag | Cut every wire crossed |
| `Shift` + right drag | Add a reroute on the wire crossed |
| Right-click a card, a group frame, a link, or the canvas | Context menu |

Mute, collapse, and dissolve are context-menu commands in this editor and bind no key. A group's own
`Rename` is likewise a menu command and a double-click: `Enter` keeps its one meaning, so it never
becomes "rename whichever thing is selected".

### Header menus (task NODES-1)

Add, View, Select, and Node sit in the panel's own header, right after the switcher: the panel's
menu bar, not the application's. Add is the same categorized submenu the canvas's own right-click
menu offers -- one surface, restated in two places, never a second opinion about what is addable.
View, Select, and Node reuse the canvas's existing commands and objectNames wherever one already
existed (`nodeFitAction`, `nodeSelectAllAction`, `nodeGroupAction`, and so on); a command the
context menu shows only when it applies, this persistent menu instead disables when it does not,
since hiding and re-showing entries in a menu that stays open across gestures would read as the
menu itself changing shape. Frame Selected, Grid Snapping, Link Style, Select None and Select
Invert bind no new keys; Arrange uses the two explicit node-canvas bindings below:

| Menu | New command | Does |
| --- | --- | --- |
| View | Frame Selected | Frames exactly the selected cards; frames the whole graph if nothing is selected |
| View | Grid Snapping | Toggles grid snapping (see below); the same toggle the footer's switch offers |
| View | Link Style | Spline / Straight / Angled (see below); the same choice the footer's dropdown offers |
| View → Organize | Arrange All | Lays out every node, fits the graph, and commits one `MoveNodes` transaction |
| View → Organize | Arrange Selection | Lays out the selected induced subgraph in place and commits one `MoveNodes` transaction |
| Select | None | Clears the selection |
| Select | Invert | Selects every unselected node, deselecting every selected one |
| Select | Linked Upstream | Extends the selection to every node reachable by following links backward, transitively |
| Select | Linked Downstream | The same walk, following links forward |

A shortcut shown next to a header menu item (Fit, Actual Size, Select All, Group, Ungroup, Delete,
Arrange All, Arrange Selection)
is the SAME key `NodeGraphicsView` already claims through `ShortcutOverride` -- it is display text,
not a second live binding, so it never fires while some other panel has focus (see Ownership
Boundary below). The header itself never wraps to a second row: once it is too narrow to hold all
four menus, they collapse into a single "..." overflow menu holding the same four as submenus.

### Link style (task NODES-1)

View > Link Style picks how every wire is drawn, persisted in `QSettings` under `nodes/link-style`:

| Style | Drawn as |
| --- | --- |
| Spline (default) | The original cubic bezier |
| Straight | A direct line, socket to socket |
| Angled | Orthogonal: horizontal, then vertical, then horizontal |

Changing it repaints every wire already on the canvas, including the drag preview link; hit-testing
always follows whichever path is actually drawn, so cutting, hovering, and picking up a wire work
identically under every style.

### Grid snapping (task NODES-1)

View > Grid Snapping (also in the footer, as a switch) snaps a dragged card's position to a fixed
lattice -- `QSettings` `nodes/snap` (off by default) and `nodes/grid-size` (16 design px) -- both
while the drag is in flight and on the position `MoveNodes` actually commits, so a released card
never lands one pixel off the grid it appeared to land on. **`Alt` held bypasses it** for that one
drag, without touching the persisted setting. The painted dot grid steps by the same size, so what
snapping targets is always what is visibly drawn. A plain click -- a press and a release with
nothing dragged in between -- is never affected: only a card a `mouseMoveEvent` actually moved is
eligible to snap on release.

Every socket is a drag target, not only the image ports: a node's parameter roles are sockets of their
own kind, so the same one gesture that wires an image wires a value into a parameter. Nothing new was
added for it -- a link into a parameter socket records that parameter's driver binding, and releasing
the same drag on empty canvas restores the parameter's registered default. The drag's own preview says
in advance whether a release will be accepted: a compatible socket brightens and an incompatible one
dims, and the wire turns `Error` red over a socket whose kind the connect rule would refuse. The rule
it previews is the same one the command applies, promotions included, so the preview is never a second
opinion.

Merge is available in Add menus and search. Drop any image output on its pill to insert at the
caret; drop on the body to append. Drag an existing link end within the pill to reorder its stable
slot, or away to disconnect. Only Output and its directly connected Merge are protected from Delete.

## Timeline

The timeline follows the Merge directly feeding Output. Direct Layer inputs remain editable;
nested Merges appear as one collapsed composition-colored row, with enabled as the only editable
layer toggle. Properties on a Merge lists its own inputs in order, with Layer blend/opacity editable
and plain-image values read-only. Solo on a Layer applies composition-wide, including nested Merges.

Space uses the shared window transport, even with no Timeline panel open. Middle-drag pans the
Viewer and Nodes; Space no longer arms a pan gesture.

| Binding | Action |
| --- | --- |
| Click layer chevron / collapsed key summary | Expand or collapse that layer / expand it to expose parameter lanes |
| Inline parameter value / diamond | Commit through the Properties session setters / use the shared animate, add-key, remove-key gesture |
| Click an upstream group chevron | Collapse or expand that value node's rows; the state is per layer and per node |
| Inline value / diamond / key on an upstream row | Edit the VALUE NODE's own parameter, key it, and drag its keys exactly as a layer parameter's |
| Click a driven row's driver link | Select the driving node and frame it in the node canvas |
| Click key / Shift-click key | Select one / extend the session key selection across lanes |
| Drag empty parameter lane / Shift-drag empty lane | Box-select keys across rows / add the box contents to the selection |
| Drag selected key | Move the selected keys together; snap to frames, other keys, playhead and work-area edges |
| Shift during key drag | Disable frame and magnetic snapping; translate the selection by one rational subframe offset |
| Alt-drag one selected key | Duplicate it at the destination with a new stable key ID |
| Alt-drag first or last of at least two selected keys on one parameter | Stretch the selected times about the opposite endpoint, snapped to frames |
| Double-click key / animated lane background | Move playhead to the key's exact time / insert a sampled key at the clicked frame |
| `Delete` / `Backspace` with parameter lanes focused | Delete the key selection in one transaction |
| `Ctrl+C` / `Ctrl+V` with parameter lanes focused | Copy selected keys / paste onto the same parameters with the earliest copied time at the playhead |
| Right-click key | Hold, Linear, Ease In-Out, or Delete for the complete key selection |
| `Escape` during key drag or box selection | Cancel the gesture without a document edit |
| `B` / `N` | Set work-area start / exclusive end at the playhead |
| Drag work-area grips / double-click strip | Set frame-snapped range / reset to full duration |
| Drag bar edge / body | Trim / move both endpoints; commit once on release, Escape cancels |
| `Shift` during bar drag | Disable magnetic snapping; committed endpoints still land on frames |
| `Ctrl+K` | Split selected layers at the playhead (also Timeline Edit) |
| Click / Ctrl-click / Shift-click row | Select one / toggle membership / extend contiguous selection |
| Drag row | Reorder stable slots at the insertion indicator |
| Double-click name | Inline rename; Return commits, Escape cancels |
| Right-click row | Duplicate, Delete, Rename, Blending, Label Color, Split at Playhead |
| `Ctrl+wheel` over ruler or lanes | Zoom time about the pointer; the pointer's time stays fixed until a composition boundary clamps the range |
| `Shift+wheel` or horizontal wheel | Scroll the visible time range |
| Drag the navigator window | Scroll; drag either window edge to resize the visible range; `Escape` cancels |
| `Ctrl+0` | Timeline View → Zoom to Fit |
| `Ctrl++` / `Ctrl+-` | Timeline View → Zoom In / Zoom Out about the visible center |
| `Ctrl+A` / `Ctrl+Shift+A` | Timeline Select → All / None (shared layer-boundary selection) |
| `Delete` / `Backspace` with layer stack or lanes focused | Timeline Edit → Delete Layer, through `RemoveNodes` on the selected layers' Layer nodes |

The header offers **Add** (Solid, Text), **View**, **Edit**, and **Select** menus. Edit reuses the
application's Undo/Redo actions and shortcuts. Delete in a keyframe lane retains its keyframe
behavior. Menus collapse into one `…` popup when their available header width is too small.

View → Frames / Timecode changes the ruler's labels, and persists the preference in
`timeline/time-format` -- the same key the Viewer footer's own readout reads and writes, so the two
surfaces cannot disagree about the format. Frames is the default. Timecode is non-drop `HH:MM:SS:FF`,
using the nearest nominal integer frame rate for fractional rates; exact composition seconds remain
visible beside either format. This is a display preference, with no effect on document or render time.

Time navigation is editor-local session state. The visible range is half-open `[t0, t1)`, clamped
inside the full composition duration, with a minimum span of one frame (or the entire duration for
a shorter composition). Zoom to Fit restores the full duration. All lane, keyframe, work-area,
playhead, and cache projections use this same range. Plain vertical wheel scrolling retains the
shared layer-stack/lane vertical scroll.

RAM Preview sits in the transport cluster as a button, but its KEYS are declared by the Composition
menu, not here: one `Qt::WindowShortcut` owner per sequence, or Qt reports an ambiguous overload and
fires neither. The button, the menu item, and `Ctrl+Shift+Space` all call the one
`RamPreviewController::toggle()`, per **Ownership Boundary** below.

Bar magnetic targets are other layers' in/out points, the playhead and work-area edges. A guide
marks the chosen target. Empty lane space retains ruler scrubbing. Visibility, solo and lock cells
commit their boundary commands; audio and Parent columns stay hidden. A label menu offers eight
presets, a custom RGB picker and Kind Default. Name editing consumes text/navigation keys before
Timeline commands. Parenting remains separate work.

New object names: `timelineSetWorkAreaStartAction`, `timelineSetWorkAreaEndAction`,
`timelineSplitLayerAction`, `timelineLayerInsertionIndicator`, `timelineLayerRenameEditor`,
`timelineLayerContextMenu`, `timelineLayerLabelColorDialog`. Existing object names are retained,
including the hidden `layerParentDropdown`.

Expanded layers contain Transform (Position, Anchor, Scale, Rotation), Appearance (Opacity,
Blending), the source node's animatable parameters (Solid Color; Text Size and Color), and one
collapsible group per value node driving any of them, titled by that node's display name and holding
that node's animatable parameters as ordinary rows. Each parameter occupies one row; vector
components share that row, up to three. A driven parameter row shows its driver's name behind a link
glyph and the resolved value read-only, with no editor and no diamond -- a driven parameter has no
key of its own, and the key that moves it is the upstream row's. Expansion is per-layer editor state,
cleared on composition changes, and is not saved. Collapsed summaries deduplicate coincident key
times across the boundary, source and upstream parameters and only expand the layer when clicked.

A click on an already selected key keeps the set during a possible drag, then selects that one key
on release if no drag occurred. Right-clicking a selected key preserves the set. Locked layers reject parameter and key edits. Batch gestures
commit once on release; a stale revision cancels the gesture. A collision rejects the complete
move, stretch, duplicate or paste. A stretch keeps its endpoint on the same side of its anchor;
rounding that would merge keys rejects the edit. The fixed endpoint retains its exact time,
including a subframe time; only the moved keys snap. Ordinary moves clamp the whole selection to the
composition, preserving its exact spacing. The dragged key snaps to a frame or an exact magnetic
target; the other selected keys retain their rational offsets. Shift subframe pointer offsets use nanosecond resolution and
checked rational addition. Clipboard offsets remain exact, including a subframe playhead; the
clipboard is session-local and clears when the composition or document changes.

Final keys keep canonical Linear outgoing interpolation when a batch interpolation is applied.
Deleting every key on a parameter restores a constant equal to its earliest key's value. Both
choices, and all affected curves, are restored exactly by one Undo. The same Qt gestures and
command path apply on Linux, macOS and Windows; platform qualification remains a separate gate.

The retained `timelineKeyframeArea` and `timelineKeyframePanel` now identify the lane container
inside `timelineLaneRegion`, with no separate panel beneath the layer stack. The former
`timelineKeyframeIndent` and `timelineKeyframeScrollGutter` wrappers are removed with that separate
layout. New object names are `timelinePropertyRow`, `timelinePropertyLabel`,
`timelinePropertyDiamond`, `timelinePropertyValue`, `timelinePropertyBlending`,
`timelinePropertyColor`, `timelinePropertyComponent`, and `timelineKeyframeRow`. The driven display
adds `timelinePropertyDriven`, `timelinePropertyDriverLink` and `timelinePropertyDrivenValue`.

## Properties

| Gesture | Action |
| --- | --- |
| Type in header search | Filter labels by case-insensitive substring; hide empty sections |
| Escape in search | Clear query, restore rows and return focus to the panel |
| Scrub a numeric row | Preview locally; commit once on release; cancellation writes nothing |
| Click color / text disclosure chevron | Reveal detailed color components / multiline text editing |
| Leave a multiline editor | Commit the complete text as one parameter edit |
| Click section-header jump icon / driver link | Select the target and frame it in the node canvas |
| Right-click parameter row or control | Reset to its registered default; driven reset disconnects with Undo support |
| Wheel over the body | Scroll cards beneath the panel header search |
| Click an anchor grid dot | Set anchor offset from evaluated local bounds, one undoable command |
| Edit Anchor X/Y | Highlight a matching grid point; clear highlight for a custom offset |
| Arrow keys on the anchor grid | Move to a neighbouring grid point |
| Click text stepper arrow | Increase/decrease the field by one step |
| Click alignment segment | Set Left / Center / Right through the registered parameter |

Upstream rows target their own parameter IDs; editing or keying them preserves the current
selection. Driver results update from ONE cancellable background evaluation owned by the session and
read by every surface that shows a driven value, at the current session time, so the Properties row
and the timeline row for one parameter always show the same string. A driven String shows its
resolved text read-only, exactly as every other kind shows its resolved value. A stale result cannot
overwrite a newer selection, snapshot, or time. Collapsed section state
is a UI preference; searching temporarily exposes matches without changing that preference.

## Retired Bindings

These were bound in earlier slices and are bound by nothing now. They are listed so that a future
slice does not reintroduce one by accident, and so an artist reading an older note knows what replaced
it.

| Retired | Was | Replaced by |
| --- | --- | --- |
| `F` | Fit, in the Viewer and the Nodes canvas | `Ctrl+0` |
| `Z` | Actual size, in the Viewer and the Nodes canvas | `Ctrl+1` |
| `X` | Remove the selection (Nodes) | `Delete` / `Backspace` |
| `M` | Toggle mute (Nodes) | Context menu only |
| `H` | Toggle collapse (Nodes) | Context menu only |
| `Shift+D` | Duplicate (Nodes) | `Ctrl+D` |
| `Ctrl+X` | Dissolve (Nodes) | Context menu only |
| `Shift+A` | Open the Add search (Nodes) | `Tab` |

## Ownership Boundary

The bindings in an editor belong to that editor's own widget: `NodeGraphicsView` claims the node
canvas's keys through `ShortcutOverride` so a window-level shortcut cannot take one, and `ViewerEditor`
claims its own. A command reached from both the keyboard and a menu is one named method called by both,
never a menu item that synthesizes a key press — a menu that worked that way could only ever offer what
the keyboard happened to bind.
