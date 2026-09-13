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
- **A modifier-free letter is a tool, not a command.** Commands that destroy, duplicate, or toggle
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
| `Ctrl+Shift+Space` | RAM Preview: cache this composition's range, then play it |
| `Esc` | Cancel the RAM preview being cached (bound only while one is) |
| `F11` | Full screen |
| `` ` `` | Maximize or restore the panel under the pointer |
| `Alt` (held) | Reveal menu mnemonics |

## Viewer

| Binding | Action |
| --- | --- |
| `Ctrl+0` | Fit |
| `Ctrl+1` | Actual size (100%) |
| `Space` (held) + left drag | Pan |
| Middle drag | Pan |
| Wheel | Zoom about the pointer |
| `Esc` | Cancel the drag in flight |

## Nodes

| Binding | Action |
| --- | --- |
| `Ctrl+0` | Fit the graph |
| `Ctrl+1` | Actual size (100%) |
| `Home` | Fit the graph (alias) |
| `Space` (held) + left drag | Pan |
| Middle drag | Pan |
| Wheel | Zoom about the pointer |
| `Tab` | Open the Add search at the pointer |
| `Ctrl+A` | Select all nodes |
| `Delete` / `Backspace` | Remove the selection |
| `Ctrl+D` | Duplicate the selection and start placing it |
| `Ctrl+G` | Group the selection in a node group |
| `Ctrl+Shift+G` | Ungroup every node group the selection is in |
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
menu itself changing shape. A few commands are new here and bind no key, matching the "reserved
keys are left unbound" rule above:

| Menu | New command | Does |
| --- | --- | --- |
| View | Frame Selected | Frames exactly the selected cards; frames the whole graph if nothing is selected |
| View | Grid Snapping | Toggles grid snapping (see below); the same toggle the footer's switch offers |
| View | Link Style | Spline / Straight / Angled (see below); the same choice the footer's dropdown offers |
| Select | None | Clears the selection |
| Select | Invert | Selects every unselected node, deselecting every selected one |
| Select | Linked Upstream | Extends the selection to every node reachable by following links backward, transitively |
| Select | Linked Downstream | The same walk, following links forward |

A shortcut shown next to a header menu item (Fit, Actual Size, Select All, Group, Ungroup, Delete)
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

## Timeline

Transport bindings are unchanged by this document.

| Binding | Action |
| --- | --- |
| `Space` | Play / pause |
| `Left` / `Right` | Step one frame |
| `Home` / `End` | Go to start / end |

RAM Preview sits in the transport cluster as a button, but its KEYS are declared by the Composition
menu, not here: one `Qt::WindowShortcut` owner per sequence, or Qt reports an ambiguous overload and
fires neither. The button, the menu item, and `Ctrl+Shift+Space` all call the one
`RamPreviewController::toggle()`, per **Ownership Boundary** below.

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
