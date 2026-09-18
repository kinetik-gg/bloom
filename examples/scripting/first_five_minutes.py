"""The first five minutes in the Script editor: add a solid, key it, render a frame, undo.

Run it with `bloom-cli run examples/scripting/first_five_minutes.py`, or paste the lines one at a
time into the Script editor. Every call here omits the composition and the time: both default to
what `bloom.context` is looking at.
"""
import pathlib
import tempfile

import bloom

# 1. Look around. Proxies print as what they are, not as internals.
print(bloom.context)
print(bloom.context.composition, bloom.context.time)

# 2. Add a solid. The composition comes from context; name and colour are yours.
#    `help(bloom.ops.layer.add_solid)` prints the arguments, the defaults and an example.
result = bloom.ops.layer.add_solid("Red", (1, 0, 0, 1))
layer = next(output["id"] for output in result["outputs"] if output["name"] == "layer")
position = next(output["id"] for output in result["outputs"]
                if output["name"] == "positionParameter")
print(bloom.data.nodes(bloom.context.composition)[-1])

# 3. Key its position X across the first second, as one undo step.
#    Times accept a whole frame, a Fraction or an exact (numerator, denominator) pair.
with bloom.transactions.group("Slide the solid"):
    bloom.ops.animation.create_for_parameter(position, time=0)
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
        position, 0, 200.0, time=0)
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
        position, 0, 1200.0, time=24)

# 4. Render one frame. The destination directory must already exist.
with tempfile.TemporaryDirectory(prefix="bloom-first-five-minutes-") as directory:
    frame = bloom.render.frame(12, out=pathlib.Path(directory) / "frame-12.png")
    print(frame.files[0].path, frame.files[0].digest)

# 5. Undo. One transaction is one undo step, and the solid is one more.
bloom.transactions.undo()
bloom.transactions.undo()
print("layer", layer, "is gone:", len(bloom.data.nodes(bloom.context.composition)))
