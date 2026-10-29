# The Context Bar

The context bar is the row directly under the menu bar
([ADR 0031](../decisions/0031-contextual-tool-bar.md)). It holds the tools on the left, the active
tool's options next to them, and actions on the selected layers on the right. Window > Context Bar
hides or shows it; Bloom remembers the choice.

Nothing the bar does is different from doing it anywhere else: every option and action is the same
undoable command Properties, the Timeline and the menus use.

## Tools

There is one active tool for the whole application, as in After Effects. Choosing a tool in the bar,
or with its key while a Viewer has focus, changes it for every Viewer; the tool then acts in
whichever Viewer you use it in.

| Tool | Key | Use |
| --- | --- | --- |
| Select | V | Select and move layers; edit a path's or mask's anchors |
| Hand | H | Drag to pan |
| Zoom | Z | Click to zoom in, Alt-click to zoom out |
| Rectangle, Ellipse, Polygon, Star | R, E | Drag a shape (Shift constrains, Alt draws from the centre) |
| Line | | Drag a line (Shift snaps to 45 degrees) |
| Pen | P | Click anchors, drag handles; Enter or Escape finishes |
| Text | T | Click to place text and type |
| Cryptomatte | | Click objects of the selected Cryptomatte to add them; Alt-click removes |

Rectangle, Ellipse, Polygon and Star share one button that shows the last shape you chose. Press and
hold it, or right-click it, to choose another; the small mark in its corner says there are more.

Creation tools are one-shot: once you draw a shape, a mask, a path or text, Bloom returns to Select,
so the next click selects instead of drawing again. A click that does not drag creates nothing and
keeps the tool. Escape confirms what you are doing and returns to Select; Undo is how you discard
it. Properties' "Pick in Viewer" for a Cryptomatte chooses the Cryptomatte tool the same way, and it
stays chosen for further picks until you choose another tool.

## Tool options

The middle of the bar shows options for the active tool. With a matching layer selected, they edit
that layer; with none, they set what the next one starts with.

- **Select and Pen:** the handle mode of the selected anchors on a path or mask: Corner, Mirrored,
  Aligned, Free or Auto. With anchors of different modes selected, no mode is shown; choosing one
  converts them all. With no anchors selected the control is unavailable.
- **Shapes, Line and Pen:** Fill and its colour, Stroke and its colour, and the stroke width. A line
  always has a stroke and no fill.
- **Text:** the font, the size and the alignment (left, centre or right).

An option that does not apply is greyed out where it always is, and its tooltip says why. After you
type into a field in the bar and press Enter or Escape, the keyboard returns to the Viewer.

## Align and distribute

- **Align** lines up the selected layers' left edges, horizontal centres, right edges, top edges,
  vertical middles or bottom edges.
- **Distribute** spaces the selected layers' centres evenly, horizontally or vertically. It needs
  three or more layers.
- **Align To** chooses what they line up with:
  - **Selection** (the default): the box around all the selected layers. Distributing keeps the
    two outermost layers where they are. Aligning needs two or more layers.
  - **Composition**: the composition frame. Distributing moves the outermost layers to the frame's
    edges.
  - **Key Object**: the primary selected layer, named in Align To, which stays where it is.

The same actions are in Composition > Align.

Bloom measures what you see at the current time: each layer's visual bounds after its transform,
its parent's transform and its content, taken from the frame the Viewer is showing. While the
Viewer is still catching up with an edit or a new time, the actions wait, greyed out. A parented
layer moves in its parent's space, so it lands where the bounds say even under a rotated or scaled
parent. A layer whose Position is animated gets a keyframe at the current time; its other keyframes
stay. Locked layers, and layers whose Position is driven by a node, do not move, and the status line
says which and why. One Undo reverses the whole action.

The same actions are scriptable as `bloom.ops.layer.align_layers` and
`bloom.ops.layer.distribute_layers` ([Scripting Bloom with Python](python.md#align-and-distribute)).

## Narrow windows

When the window is too narrow for the whole bar, groups fold into the » button at the end of the
tool options, from the right: Distribute first, then Align To, then Align, then the tool options.
The tools themselves always stay.
