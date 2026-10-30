# The Viewer

The Viewer shows the composition at the playhead and is where you move, scale and rotate layers
directly. Every edit is the same undoable command Properties and the Timeline use.

## Moving layers

With the Select tool, drag a selected layer to move it. The drag is one undo step, committed when
you release the mouse.

Hold **Shift** while dragging to lock the move to a fixed axis: horizontal, vertical, or one of the
two 45 degree diagonals. Bloom picks the axis nearest to the direction you have dragged since you
pressed the mouse.

- Press or release Shift at any moment during the drag. The layer never jumps: it always sits where
  a constrained (or free) move from the original position would put it.
- The axis is the one you see on screen, at any zoom or pan, even for a rotated or parented layer.
- The same lock applies when you drag points of a path or mask.
- Shift still adds to the selection when you click, scales proportionally when you drag a corner
  handle, and steps rotation by 15 degrees. Shift only constrains a move that is already dragging.

## RAM Preview

Click **RAM Preview** in the Viewer footer, choose Composition > RAM Preview, or press **Numpad 0**
(or Ctrl+Shift+Space) to cache the composition's range into RAM and then play it. While it fills,
the button reads **Cancel** with the fill progress; click it again, press the same key, or press
**Esc** to stop. Frames already cached are kept, and the button returns to RAM Preview. Numpad 0
does nothing while you type in a text or value field.
