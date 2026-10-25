# Interaction Model

Status: accepted

Updated: 2026-10-25

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
| `Ctrl+Alt+N` | New Composition… |
| `Ctrl+Alt+K` | Composition Settings… for the active composition (After Effects' `Ctrl+K` splits layers here) |
| `Space` | Play/pause immediately from cache in any panel; focused text entry keeps Space |
| `Ctrl+Shift+Space` | RAM Preview: cache this composition's range, then play it |
| `Esc` | Cancel the RAM preview being cached (bound only while one is) |
| `F11` | Full screen |
| `` ` `` | Maximize or restore the panel under the pointer |
| `Alt` (held) | Reveal menu mnemonics |

New project (`Ctrl+N`) and application startup begin **blank**: the project owns no composition, so
every editor shows its empty state and the composition-dependent commands (Duplicate, Rename,
Delete, Export Frame) are disabled. New Composition is the way to author the first one; creating it
activates it. Deleting the final composition returns to that same blank project as one undoable
command, and undo restores and reactivates the composition.

## Viewer

Selected layers show their delivered-frame bounds polygon and anchor. Position moves the anchor in
composition space; anchor is an offset from local content centre, so changing it with position held
fixed moves content. The overlay follows zoom/pan and shows the actual delivered frame during a stale
preview. Cached display frames retain evaluated geometry without their Float32 process images, so
playback also shows the overlay without re-rendering. Empty content or another composition has no overlay. Selecting node cards can show multiple layer boundaries. Existing drag bindings are unchanged.

Passive picking uses the actual published image, destination and evaluated bounds together. A
pending selection outline or a different project/composition/time has no passive hit geometry.
Once a gesture begins, its mapping stays frozen through preparation/presentation lag and admission
lease expiry. Actual time, composition format, viewport/transform, selection, domain or lifecycle
changes, mouse-ungrab and window deactivation cancel it. Escape confirms everywhere in the Viewer:
a move, rotate, scale or anchor-point gizmo drag keeps the change made so far as one undoable
command, a region-of-interest drag keeps its region, and the Viewer returns to Select.
Creation tools are one-shot and Escape confirms rather than discards: committing a shape, a mask on
the selected layer, a Pen path or text returns the Viewer to Select, so the next click outside
selects or deselects instead of creating another object. Escape commits a mid-drag shape or mask at
its current size, finishes a Pen path with the anchors placed so far (a Pen mask closes), keeps
typed text, and keeps an edit to existing text or anchors as one command; Undo is the discard, one
step per created or edited object. Empty text, a Pen draft below its minimum and a zero-size press
create nothing; a zero-size press keeps the tool. `ui-grammar.md` owns the full tool rules.
Pending or failed viewer EV/gamma changes may keep the prior complete image visibly stale. The
window status follows the active visible Viewer, then the last-active visible Viewer, then a stable
first visible Viewer. Hiding or replacing that Viewer switches or clears its probe readout. With no
visible Viewer, the window makes no displayed qualification claim.

| Binding | Action |
| --- | --- |
| Composition selector | Switches the shared session to the chosen document composition and clears selection/time-local state as usual |
| Object selector | Selects the chosen current-composition layer; `None` clears selection |
| View → Fit / Actual Size / Zoom In / Zoom Out | Uses the viewer transform and shows the existing shortcut beside each item |
| View → Safe Areas / Centre Cross / Thirds / Rulers / Pixel Grid | Toggles display-only guides; Safe Areas uses the active composition's stored percentages |
| View → Safe Area Preset | Applies Broadcast, HD, Cinema, Social, or validated Custom percentages in one undoable composition edit |
| Select → All / None / Invert | Selects, clears, or inverts the current composition's layer boundaries |
| Composition menu / selector trailing menu | New Composition, Composition Settings, Duplicate, Rename, and Delete Composition; Composition Settings, Duplicate, Rename, and Delete are disabled while no composition is active, and Delete may remove the final composition (returning to the blank project) |
| `Ctrl+0` | Fit |
| `Ctrl+1` | Actual size (100%) |
| Middle drag | Pan |
| Wheel | Zoom about the pointer |
| `Esc` | Confirm whatever is in flight and return to Select: a gizmo move/rotate/scale/anchor drag, a region of interest, a creation, a text edit or a path/mask anchor edit (see below); Undo discards |
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

Cryptomatte (#364). Selecting a Cryptomatte shows its ID-colour preview in the Viewer: every object
in its own colour, the selected mattes brightened (Properties "Preview", on by default, turns it
off). The Viewer's eyedropper tool, or "Pick in Viewer" in Properties, picks objects: a click or
Shift-click adds the object under the cursor, Alt- or Ctrl-click removes it, each pick one undoable
edit. The point is mapped through the displaying layer's transform and time, so a moved, scaled,
rotated, parented or retimed layer picks what is shown; a point outside the source reports "No
object under the cursor". Properties also lists the manifest names as a searchable checklist, the
selected names with remove buttons, and a field for a name or a raw `<ID>`.

Masks (ADR 0028). With a layer selected, the Rectangle, Ellipse and Pen tools add a mask to that
layer instead of creating a shape layer: Rectangle draws four corner anchors, Ellipse four anchors
with circular handles, and Pen the clicked path, all mapped into the layer's content space through
the inverse of its evaluated content-to-composition transform (a layer with no evaluated bounds at
the playhead drops the gesture). With nothing selected, the tools behave as before. The selected
mask draws its outline, anchors and handles and dashed feather bounds at expansion ± feather / 2;
a mask and a Pen shape path share one editor: anchor and handle drags, multi-selection (Shift-click
and marquee), the five handle modes (Corner, Mirrored, Aligned, Free, Auto) from Alt gestures, the
anchor mode strip and the context menu, and Pen add, delete and continue
(`ui-grammar.md` owns the details). Every edit previews live in the Viewer (a session-only override
of the path, so the document never changes mid-drag) and commits one path edit on release (a key at
the playhead on the layer's own clock when the path is keyed), and clicking an outline with the Select tool selects that mask.

## Assets

Assets is the project panel (#378), after the After Effects Project panel: one tree of folders,
compositions and imported assets with columns, a search field and a selection header
(`ui-grammar.md` owns the presentation). Its keys belong to the focused tree, which claims them
through `ShortcutOverride` so a window-level shortcut cannot take them; text entry stays in the
search or inline rename field.

| Gesture or control | Action |
| --- | --- |
| Search field text | Filters the tree by name and tag, case-insensitively; `tag:` matches tags only; matching rows keep their folders visible |
| Click a column header | Sorts by that column, again to reverse; folders stay first |
| Drag a header divider / header | Resizes / reorders a column |
| Right-click the header, View > Columns | Chooses the visible columns; the choice is a per-user setting |
| View > Expand All / Collapse All | Expands or collapses every folder |
| Add > New Composition…, footer New Composition | Opens the Composition dialog in create mode and adds one composition, filed in the selected folder (see "Composition Dialog" below) |
| Add > New Folder, footer New Folder | Creates a folder in the selected folder and starts its inline rename |
| Add > Import…, footer Import, File > Import | Imports into the selected folder: a selected folder, or the folder of the selected item; the root with nothing selected |
| Select > All / None | Selects all visible rows or clears the selection |
| Click, Shift-click, Ctrl-click; Up/Down/Left/Right, Home/End | Selects one row, a range or a set; arrows move and Left/Right collapse or expand folders |
| F2 or Return | Renames the current row in place (asset, composition or folder); Escape cancels |
| Delete or Backspace, footer Delete | Deletes the selection; a used item, or a folder that holds items, asks first (see below) |
| Double-click a composition | Switches the shared `CompositionSession` to that composition, as in After Effects; it opens no dialog. Double-clicking a folder expands it |
| Click a Tags cell | Filters by the asset's first tag |
| Drag rows onto a folder, onto a row, or onto empty space | Moves the dragged assets, compositions and folders into that folder, the row's folder, or the root, as one undoable step; a folder never moves into itself or a subfolder |
| Drag rows to Timeline, Viewer or Nodes | Adds the first asset (or composition when no asset is dragged) as before |
| Drop files or folders onto Assets | Imports them into the folder under the pointer, or the root on empty space |
| Drop files or folders onto Timeline or Nodes | Imports them into the selected Assets folder; no layer is added |
| Right-click an asset | New Composition from Selection, Input Colour…, Replace Footage…, Reload Footage, Reveal in File Manager, Rename, Edit Tags…, Delete, then New Folder, New Composition…, Import… |
| Right-click a composition | Open, Composition Settings…, Rename, Duplicate, Delete, then the same three |
| Right-click a folder | Rename, Delete, then the same three; empty space offers only the three |

New Composition from Selection makes one composition named and sized from the first selected
footage item, with a video's frame rate and duration, a sequence's frame count or an audio file's
duration (the active composition supplies anything the footage does not state). It holds a layer
for every selected image, sequence, video or audio asset, the first on top, is filed in the clicked
item's folder, opens, and is one undo step. Reload Footage probes the asset's own file again on the
import worker, as a relink to the same path. Reveal in File Manager opens the folder holding the
file.

Every import from the rows above (and a dropped folder, which contributes the media files below it)
shows the import progress modal (#377, [ADR 0029](../decisions/0029-import-progress-and-cancellation.md)).
It appears after 300 ms, so a quicker import shows nothing; until then window input is held back, so
the import blocks the window from its start. The modal blocks the application and shows the overall
bar, `Importing 3 of 12` (a sequence counts once), the file in progress (`Pulse.0001.png · 120
frames found` while a sequence is scanned, `Looking in <folder>` while a folder is searched) and
Cancel. The preparation runs on the import worker throughout.

| Gesture or control | Action |
| --- | --- |
| Cancel, Escape, closing the modal | Stops at the next safe point: the file in progress is abandoned, everything already imported is kept as one undoable step. The heading reads `Cancelling…` until the worker stops |
| Return, Escape or Close on the summary | Closes it |

An import that added everything closes the modal and shows `Imported N assets` in the status bar.
Otherwise the modal becomes a summary: `Import finished`, `Import cancelled` or `Nothing was
imported`; the counts (`Imported 10 · Skipped 2 · Not imported 9`); a note when something was kept
after Cancel (one Undo removes it) or why nothing was added; and each skipped file with its plain
reason, including files the project already holds. A quick import that skipped anything shows the
summary at once. The status bar still names the first skipped file and Jobs keeps every reason.

Deleting unused items or empty folders is immediate. Deleting an item that is used, or a folder that
holds items, opens a warning first: one used item names how often and in how many compositions it is
used; a folder states how many items it holds and how many of them are used, and offers Delete
Folders Only (assets, compositions and subfolders move up to the parent) or Delete with Contents.
Cancel deletes nothing. Every delete is one transaction and one undo step; deleting the active
composition switches to the remaining one with the lowest id, and deleting the final composition
returns to the blank project.

### Composition Dialog

New Composition and Composition Settings are one modal (#376), titled after its mode, with a Basic
section and a collapsed Advanced section. Create mode starts from the active composition's settings and commits
`AddCompositionWithSettings` ("Add Composition"); settings mode edits one composition and commits
`SetCompositionSettings` ("Composition Settings"). Each is exactly one undoable command, and
unchanged settings commit nothing. Entry points: Composition menu (`Ctrl+Alt+N`, `Ctrl+Alt+K`), the
Viewer composition menu, and the Assets Add menu, footer and context menus.

| Control | Behaviour |
| --- | --- |
| Name | Required |
| Preset | Custom, the user's presets, then HD 1080p (1920 × 1080), HD 720p (1280 × 720), UHD 4K (3840 × 2160), DCI 2K (2048 × 1080), DCI 4K (4096 × 2160), Vertical 1080 × 1920 and Square 1080, all square pixels. Under the preset a muted summary reads the current values, for example `1920 × 1080 · 16:9 · Square pixels · 24 fps · 10 s (240 frames)`: the frame aspect (size times pixel aspect, reduced, or a ratio to one at two decimals such as `1.9:1` when its terms exceed 32), the pixel aspect, the rate (with `drop-frame` when labels are drop-frame) and the duration; a part whose value is invalid is left out. Choosing a built-in sets size and pixel aspect only; choosing a user preset sets every value it holds. The field shows the first preset whose values equal the current ones (user presets first) and Custom otherwise, including while a value is invalid |
| Save as preset… / Rename preset… / Delete preset | The three buttons beside the preset (plus, pencil, trash). Save names the current values as a user preset; Rename and Delete act on the selected user preset and are disabled on Custom and built-ins. Names are trimmed, at most 128 UTF-8 bytes and unique ignoring case. User presets are a per-user application setting, never project data. A refused name is reported under the preset |
| Size and lock | Width and height in pixels; the link toggle keeps the aspect ratio while one is edited |
| Pixel Aspect Ratio | Square, D1/DV NTSC (10/11) and widescreen (40/33), D1/DV PAL (59/54) and widescreen (118/81), HDV 1080 / DVCPRO HD 720 (4/3), DVCPRO HD 1080 (3/2), and Anamorphic 2:1; a stored ratio outside the list is shown as its own entry |
| Frame Rate | One dropdown: 23.976 (24000/1001), 24, 25, 29.97 (30000/1001), 30, 48, 50, 59.94 (60000/1001) and 60 fps, then Custom. Only Custom shows the exact rate field beside it, which takes a positive integer, exact decimal (`29.97` is `2997/100`), or `numerator/denominator`; entered components must fit signed 64-bit integers and the reduced rate's components 32 bits. Typing in the Custom field stays Custom; a rate that equals a preset shows that preset whenever it is set any other way |
| Timecode: drop-frame labels | Enabled only at 30000/1001 and 60000/1001. It changes timecode labels to SMPTE drop-frame (`;` before the frames, two or four labels skipped each minute except every tenth) and never changes frames or time |
| Start | The timecode of frame 0, entered in the duration unit; stored as an exact time and labelled at the nearest frame |
| Duration and unit | Frames, Seconds (decimal or rational) or Timecode. Switching units re-displays the same exact duration; changing rate in Frames or Timecode keeps the frame count, in Seconds keeps the exact time |
| Background Colour | The composition background as a `KColorChip` at its own size with its `#RRGGBB` value beside it; the chip opens the colour picker |
| Advanced: Working Colour Space | Inherit (the project space) or a scene-linear space of the project's OCIO config |
| Advanced: Resize Anchor | Settings mode only: the Properties Anchor Point grid (`KAnchorGrid`), choosing where existing layers sit when the size changes (default centre). Click a dot or use the arrow keys |
| Advanced: Action / Title Safe | The stored safe-area percentages; title safe may not exceed action safe |

An invalid value disables OK and shows one plain reason directly under its own field (name, size,
frame rate, start, duration or title safe). A reason under Title Safe opens Advanced so it is never
hidden.

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
| Drag bar edge / body | Trim one endpoint / move the layer (start time, in and out together, so content and keys move with it); commit once on release, Escape cancels |
| Alt-drag bar body | Slip: move the start time only; the range stays, the content and keys slide inside it |
| Alt-drag bar edge | Stretch about the opposite edge: speed changes exactly by the frame ratio, the opposite edge's content time stays fixed |
| `Shift` during bar drag | Disable magnetic snapping; committed endpoints still land on frames |
| `Ctrl+K` | Split selected layers at the playhead (also Timeline Edit) |
| Click / Ctrl-click / Shift-click row | Select one / toggle membership / extend contiguous selection |
| Drag row | Reorder stable slots at the insertion indicator |
| Double-click name | Inline rename in the name label's bounds; Return or focus loss commits changed text once, unchanged text creates no transaction, Escape cancels |
| Right-click row | Duplicate, Delete, Rename, Blending, Label Color, Split at Playhead |
| Right-click empty layer-table space | Add submenu (existing Solid, Text and shape actions); direct Keyframes, Graph Editor, Snap to Frames, Zoom to Fit, Select All and Deselect All actions. Opening or dismissing the menu preserves selection |
| `Ctrl+wheel` over ruler or lanes | Zoom time about the pointer; the pointer's time stays fixed until a composition boundary clamps the range |
| `Shift+wheel` or horizontal wheel | Scroll the visible time range |
| Drag the navigator window | Scroll; drag either window edge to resize the visible range; `Escape` cancels |
| `Ctrl+0` | Timeline View → Zoom to Fit |
| `Ctrl++` / `Ctrl+-` | Timeline View → Zoom In / Zoom Out about the visible center |
| `Ctrl+A` / `Ctrl+Shift+A` | Timeline Select → Select All / Deselect All (shared layer-boundary selection) |
| `Delete` / `Backspace` with layer stack or lanes focused | Timeline Edit → Delete Layer, through `RemoveNodes` on the selected layers' Layer nodes |

The visible header offers **Add** (Solid, Text and shapes), **View**, and **Select**, in that order.
Edit remains available in the overflow menu and reuses the application's Undo/Redo actions and
shortcuts. Delete in a keyframe lane retains its keyframe behavior. Menus collapse into one `…`
popup when their available header width is too small. The blank-layer context menu reuses these
actions and styling, with only Add nested; existing layer selection is preserved until an explicit
selection command is chosen.

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

Bar gestures follow [layer-owned time](../architecture/animation-and-time.md#layer-time)
([ADR 0024](../decisions/0024-layer-owned-time.md)). Each gesture commits one `SetLayerTiming`
(undo names Move, Slip, Trim and Stretch Layer). A move keeps at least one frame of the range inside
the composition and may pass either bound; a trim may extend an endpoint outside the composition only
as far as the content extent reaches. A stretch changes speed by the exact frame ratio and keeps the
opposite edge's content time. The part of the bar outside the content extent is shaded, live during
a drag. The Properties "Time" section of the layer in context edits Start, In, Out and Frames (whole
composition frames; Start moves the layer with its content and keys), Before and After (closed
None / Hold / Loop / Ping-pong dropdowns, default None) and a Time Remap switch; it is the supported
path where a window manager claims Alt-drag. Its object names are `layerStartFrameEditor`,
`layerInFrameEditor`, `layerOutFrameEditor`, `layerFrameCountEditor`, `layerBeforeModeDropdown`,
`layerAfterModeDropdown` and `layerTimeRemapSwitch`.

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

An expanded layer with masks shows a collapsible Masks group: one row per mask labelled with its
name and mode (and Inverted), keying the path, with Feather, Expansion and Opacity rows beneath. A
row press selects the mask. Each layer row has a Matte column (`layerTrackMatteDropdown`) listing
None and the other layers of the same Merge, then Alpha, Luma and Inverted for the current matte.

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

A layer's Masks section lists each mask with a name field, a closed Mode dropdown (None, Add,
Subtract, Intersect, Difference), an Inverted switch, a keyable Path summary, Feather, Expansion and
Opacity rows, and move-up, move-down and remove buttons; touching a group selects that mask for the
Viewer. The Track Matte section has a source dropdown (None and the layers of the same Merge), an
Alpha/Luma channel dropdown and an Inverted switch. A locked layer disables both sections.

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
