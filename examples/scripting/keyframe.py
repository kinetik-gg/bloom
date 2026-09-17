"""Create a solid and key its position through one shared host transaction."""
import bloom

composition = bloom.context.composition
before = {node.id for node in bloom.data.nodes(composition)}
bloom.ops.layer.add_solid(composition=composition, name="Python animation",
                          color=(0.1, 0.5, 0.8, 1.0), position=(320.0, 240.0))
position = next(node.parameters["position"] for node in bloom.data.nodes(composition)
                if node.id not in before and "position" in node.parameters)
with bloom.transactions.group("Key Python position"):
    bloom.ops.animation.create_for_parameter(
        composition=composition, parameter=position, time=(0, 1))
    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
        composition=composition, parameter=position, component=0,
        time=(1, 2), value=400.0)
print("Position X keyed at half a second. Edit → Undo reverses the keying group.")
