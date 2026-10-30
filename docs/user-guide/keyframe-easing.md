# Keyframe interpolation, easing and layer names

A keyframe's interpolation decides how the value travels from that key to the next one. It belongs
to the key at the start of the segment, so changing a key's interpolation changes the segment that
leaves it. The last key of a curve has no outgoing segment and is always Linear.

## Interpolation modes

| Mode | What it does |
| --- | --- |
| Constant | Holds the key's value until the next key, then jumps. |
| Linear | Moves at a steady speed. |
| Bezier | A smooth curve shaped by the two keys' handles. New handles give a gentle ease in and out. |
| Sinusoidal, Quadratic, Cubic, Quartic, Quintic, Exponential, Circular | Blender's easing curves, from gentlest (Sinusoidal) to sharpest (Exponential). |
| Back | Overshoots its target and settles back, or pulls back before it starts. |
| Bounce | Bounces against the target like a dropped ball. |
| Elastic | Springs past the target and oscillates into place. |

The easing curves are Blender's, so a curve you know from Blender's Graph Editor looks the same in
Bloom.

### Ease mode

Each easing curve applies to one or both ends of its segment:

- **Ease In** eases the start of the segment.
- **Ease Out** eases the end of the segment.
- **Ease In and Out** eases both ends.
- **Automatic** is Blender's default: Ease Out for Back, Bounce and Elastic, and Ease In for every
  other curve.

### Back, amplitude and period

Three settings shape the expressive curves. They are kept on every key, so switching a key to
another mode and back never loses them.

- **Back** (Back curve): how far the value overshoots. The default is 1.70158; 0 means no overshoot.
- **Amplitude** (Elastic curve): how far the spring swings, in the property's own units. The default
  is 0.8. An amplitude smaller than the change between the two keys scales the swing down
  smoothly.
- **Period** (Elastic curve): the time between swings, in seconds. The default, about 0.17 seconds,
  is Blender's 4.1 frames at 24 fps. A period of 0 picks one automatically from the segment's
  length.

Bloom stores keyframe times in seconds, so changing a composition's frame rate never changes how an
eased segment moves.

## Easy Ease

Easy Ease works like After Effects' Easy Ease (F9): the selected keys come to a gentle stop and
start again, with 33.33% influence and zero speed. **Easy Ease In** (Shift+F9 in After Effects)
eases only the side arriving at each key, and **Easy Ease Out** (Ctrl+Shift+F9) only the side
leaving it. The eased segments become Bezier. The other end of a segment keeps the speed it had, so
easing one key does not change how its neighbors move. Easy Ease is one undo step for the whole
selection.

## Shy layers

Mark a layer **shy** to hide it from the layer table while the table's shy filter is on, like After
Effects' Shy switch. Shy never changes what renders, and turning it on or off keeps your RAM Preview
and every cached frame.

## Layer names that follow their source

A layer made from footage or from another composition is named after it: an image, video or audio
layer shows the asset's name in Assets, and a nested composition layer shows the composition's
name. Until you rename the layer, it follows its source: renaming the asset or composition, or
pointing the layer at different footage, renames the layer too.

Renaming the layer keeps your name from then on. To go back to the source's name, turn **name
follows source** back on for that layer; Bloom shows the source's current name again. Solid, text
and shape layers have no source, so they always keep the name you give them.

Projects saved before this change open with every name exactly as it was. A layer whose name was
still its source's name follows the source from then on; a layer you had renamed keeps its name.

## Python and agents

Every mode and setting is available through the same undoable operations the editor uses.

```python
import bloom

keys = [(curve, key)]                        # (curve, keyframe) or (curve, keyframe, component)
bloom.ops.animation.set_keyframes_interpolation(keys=keys, interpolation="bounce")
bloom.ops.animation.set_keyframes_easing(keys=keys, interpolation="elastic", ease="out",
                                         amplitude=2.0, period=0.25)
bloom.ops.animation.easy_ease(keys=keys, side="both")    # "both", "in" or "out"
bloom.ops.layer.set_shy(layer=layer, shy=True)
bloom.ops.layer.set_name_follows_source(layer=layer, follows=True)
```

Interpolation names are `hold` (or `constant`), `linear`, `ease-in-out` (or `bezier`), `sine`,
`quad`, `cubic`, `quart`, `quint`, `expo`, `circ`, `back`, `bounce` and `elastic`. Ease modes are
`auto`, `in`, `out` and `in-out`.

Keyframes read back through `bloom.data` with `interpolation_name`, `ease`, `back`, `amplitude` and
`period`, and layers with `shy` and `name_follows_source`. Over MCP, `query` with `kind: "layers"`
or `kind: "keyframes"` (and a `composition`) returns the same fields, and `kind: "enums"` lists the
interpolation, ease and Easy Ease names with their ids.
