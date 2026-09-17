"""Run bloom-cli with this script's absolute path from an output directory."""
import bloom

composition = bloom.context.composition
with bloom.transactions.group("Red solid study"):
    bloom.ops.project.set_name(name="Red solid study")
    bloom.ops.layer.add_solid(composition=composition, name="Red", color=(1, 0, 0, 1),
                              position=(960, 540))
result = bloom.render.frame(12, out="frame-12.png")
print(result.files[0].path, result.files[0].digest)
