# The Context Bar

Status: stub. The bar's tools and tool options are documented with the bar itself; this page covers
the alignment actions it shares with scripting.

The context bar sits directly under the menu bar ([ADR 0031](../decisions/0031-contextual-tool-bar.md)).
Window > Context Bar hides or shows it. Its right side holds actions on the selected layers.

## Align and distribute

- **Align** lines up the selected layers' left edges, horizontal centres, right edges, top edges,
  vertical middles or bottom edges.
- **Distribute** spaces the selected layers' centres evenly, horizontally or vertically. It needs
  three or more layers.
- **Align To** chooses what they line up with:
  - **Selection** (the default): the box around all the selected layers. Distributing keeps the
    two outermost layers where they are.
  - **Composition**: the composition frame. Distributing moves the outermost layers to the frame's
    edges.
  - **Key Object**: the primary selected layer, which stays where it is.

Bloom measures what you see at the current time: each layer's visual bounds after its transform,
its parent's transform and its content. A parented layer moves in its parent's space, so it lands
where the bounds say even under a rotated or scaled parent. A layer whose Position is animated gets
a keyframe at the current time; its other keyframes stay. Locked layers, and layers whose Position
is driven by a node, do not move, and the status line says which and why. One Undo reverses the
whole action.

The same actions are scriptable as `bloom.ops.layer.align_layers` and
`bloom.ops.layer.distribute_layers` ([Scripting Bloom with Python](python.md#align-and-distribute)).
