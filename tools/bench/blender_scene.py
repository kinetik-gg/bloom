"""Bloom benchmark footage: one deterministic Cycles scene, rendered headlessly.

Run with Blender 4.5 LTS:

    blender -b --factory-startup -P tools/bench/blender_scene.py -- \
        --tier 720p --out <media-root> [--device OPTIX] [--frames N] [--formats png8,png16,...]

One Cycles render per frame writes every image format: the multilayer EXR is the render output
(beauty plus depth, normal, two AOVs and Cryptomatte), and a compositor File Output node writes
the flat PNG/EXR sequences from the same denoised beauty. Everything is seeded and keyed, so a
rerun on the same GPU, driver and Blender build reproduces the same files. Another GPU may differ
in the last bits, which tools/bench/bench_media.py verify reports as `regenerated-compatible`.
"""
import argparse
import math
import os
import random
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_spec import FPS, RENDER_SETTINGS, SEED, SEQUENCE_PATTERN, TIERS  # noqa: E402

FLAT_FORMATS = {
    # id: (file_format, color_depth, exr_codec)
    "png8": ("PNG", "8", None),
    "png16": ("PNG", "16", None),
    "exr_half_piz": ("OPEN_EXR", "16", "PIZ"),
    "exr_float_zip": ("OPEN_EXR", "32", "ZIP"),
}
HUES = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 1, 1), (1, 0, 1), (1, 1, 0), (1, 0.35, 0),
        (0.45, 0, 1)]


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="blender_scene.py")
    parser.add_argument("--tier", required=True, choices=sorted(TIERS))
    parser.add_argument("--out", required=True, help="media root; files go to <out>/<tier>/")
    parser.add_argument("--device", default="OPTIX", choices=["OPTIX", "CUDA", "HIP", "METAL",
                                                               "ONEAPI", "CPU"])
    parser.add_argument("--frames", type=int, help="render only the first N frames (smoke test)")
    parser.add_argument("--formats", help="comma-separated subset of the tier's formats")
    parser.add_argument("--samples", type=int, help="override samples (smoke test only)")
    parser.add_argument("--only-frame", type=int, help="render just this frame (smoke test)")
    return parser.parse_args(argv)


def keyed(obj, path, frames_values):
    for frame, value in frames_values:
        setattr(obj, path, value)
        obj.keyframe_insert(data_path=path, frame=frame)


def linear_fcurves(id_data):
    if id_data.animation_data and id_data.animation_data.action:
        for curve in id_data.animation_data.action.fcurves:
            for point in curve.keyframe_points:
                point.interpolation = "LINEAR"
            curve.extrapolation = "LINEAR"


def node(tree, kind, location=(0, 0), **values):
    created = tree.nodes.new(kind)
    created.location = location
    for key, value in values.items():
        setattr(created, key, value)
    return created


def grain_group(width, height):
    """Base colour * (1 + amplitude * (noise - 0.5)), noise at roughly two-pixel scale."""
    group = bpy.data.node_groups.new("BenchGrain", "ShaderNodeTree")
    group.interface.new_socket("Color", in_out="INPUT", socket_type="NodeSocketColor")
    group.interface.new_socket("Color", in_out="OUTPUT", socket_type="NodeSocketColor")
    links = group.links
    inputs = node(group, "NodeGroupInput")
    outputs = node(group, "NodeGroupOutput", (800, 0))
    coords = node(group, "ShaderNodeTexCoord", (-600, 200))
    mapping = node(group, "ShaderNodeMapping", (-400, 200))
    mapping.inputs["Scale"].default_value = (width / 2.0, height / 2.0, 1.0)
    noise = node(group, "ShaderNodeTexNoise", (-200, 200), noise_dimensions="4D")
    noise.inputs["Detail"].default_value = 0.0
    noise.inputs["Scale"].default_value = 1.0
    # W is keyed linearly so the grain decorrelates frame to frame.
    w = noise.inputs["W"]
    w.default_value = 0.0
    w.keyframe_insert("default_value", frame=1)
    w.default_value = 0.73 * 239
    w.keyframe_insert("default_value", frame=240)
    centred = node(group, "ShaderNodeMath", (0, 200), operation="MULTIPLY_ADD")
    centred.inputs[1].default_value = 0.14
    centred.inputs[2].default_value = 1.0 - 0.07
    scale = node(group, "ShaderNodeVectorMath", (300, 0), operation="SCALE")
    links.new(coords.outputs["Window"], mapping.inputs["Vector"])
    links.new(mapping.outputs["Vector"], noise.inputs["Vector"])
    links.new(noise.outputs["Fac"], centred.inputs[0])
    links.new(inputs.outputs["Color"], scale.inputs[0])
    links.new(centred.outputs["Value"], scale.inputs["Scale"])
    links.new(scale.outputs["Vector"], outputs.inputs["Color"])
    linear_fcurves(group)
    return group


def material(name, grain, color=None, roughness=0.5, emission=0.0, build_color=None,
             alpha=None, build_alpha=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    tree = mat.node_tree
    links = tree.links
    bsdf = tree.nodes["Principled BSDF"]
    out = tree.nodes["Material Output"]
    if build_color:
        color_socket = build_color(tree)
    else:
        rgb = node(tree, "ShaderNodeRGB", (-700, 300))
        rgb.outputs[0].default_value = (*color, 1.0)
        color_socket = rgb.outputs[0]
    grained = node(tree, "ShaderNodeGroup", (-300, 300))
    grained.node_tree = grain
    links.new(color_socket, grained.inputs["Color"])
    links.new(grained.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = roughness
    if emission > 0.0:
        links.new(grained.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = emission
    if build_alpha:
        links.new(build_alpha(tree), bsdf.inputs["Alpha"])
    elif alpha is not None:
        bsdf.inputs["Alpha"].default_value = alpha
    aov_color = node(tree, "ShaderNodeOutputAOV", (200, -300), aov_name="bench_color")
    links.new(color_socket, aov_color.inputs["Color"])
    info = node(tree, "ShaderNodeObjectInfo", (-200, -500))
    aov_id = node(tree, "ShaderNodeOutputAOV", (200, -500), aov_name="bench_id")
    links.new(info.outputs["Random"], aov_id.inputs["Value"])
    links.new(bsdf.outputs[0], out.inputs["Surface"])
    return mat


def ground_color(tree):
    """Bricks with a fine checker and noise: high-frequency detail under camera motion."""
    coords = node(tree, "ShaderNodeTexCoord", (-1400, 300))
    brick = node(tree, "ShaderNodeTexBrick", (-1100, 400))
    brick.inputs["Scale"].default_value = 2.0
    brick.inputs["Color1"].default_value = (0.55, 0.32, 0.22, 1)
    brick.inputs["Color2"].default_value = (0.35, 0.36, 0.38, 1)
    brick.inputs["Mortar"].default_value = (0.9, 0.9, 0.85, 1)
    brick.inputs["Mortar Size"].default_value = 0.004
    checker = node(tree, "ShaderNodeTexChecker", (-1100, 100))
    checker.inputs["Scale"].default_value = 60.0
    checker.inputs["Color1"].default_value = (1, 1, 1, 1)
    checker.inputs["Color2"].default_value = (0.7, 0.7, 0.7, 1)
    mix = node(tree, "ShaderNodeMix", (-850, 300), data_type="RGBA", blend_type="MULTIPLY")
    mix.inputs["Factor"].default_value = 1.0
    links = tree.links
    links.new(coords.outputs["Object"], brick.inputs["Vector"])
    links.new(coords.outputs["Object"], checker.inputs["Vector"])
    links.new(brick.outputs["Color"], mix.inputs["A"])
    links.new(checker.outputs["Color"], mix.inputs["B"])
    return mix.outputs["Result"]


def chirp_color(tree):
    """sin(k*x^2) on the lower half, sin(k*y^2) on the upper half: a frequency sweep wall."""
    links = tree.links
    coords = node(tree, "ShaderNodeTexCoord", (-1600, 300))
    split = node(tree, "ShaderNodeSeparateXYZ", (-1400, 300))
    links.new(coords.outputs["Generated"], split.inputs[0])

    def sweep(axis, y):
        square = node(tree, "ShaderNodeMath", (-1200, y), operation="POWER")
        square.inputs[1].default_value = 2.0
        links.new(split.outputs[axis], square.inputs[0])
        scaled = node(tree, "ShaderNodeMath", (-1050, y), operation="MULTIPLY")
        scaled.inputs[1].default_value = 2600.0
        links.new(square.outputs[0], scaled.inputs[0])
        sine = node(tree, "ShaderNodeMath", (-900, y), operation="SINE")
        links.new(scaled.outputs[0], sine.inputs[0])
        level = node(tree, "ShaderNodeMath", (-750, y), operation="MULTIPLY_ADD")
        level.inputs[1].default_value = 0.42
        level.inputs[2].default_value = 0.5
        links.new(sine.outputs[0], level.inputs[0])
        return level.outputs[0]

    lower = sweep("X", 400)
    upper = sweep("Y", 100)
    half = node(tree, "ShaderNodeMath", (-750, 250), operation="GREATER_THAN")
    half.inputs[1].default_value = 0.5
    links.new(split.outputs["Y"], half.inputs[0])
    pick = node(tree, "ShaderNodeMix", (-550, 300), data_type="FLOAT")
    links.new(half.outputs[0], pick.inputs["Factor"])
    links.new(lower, pick.inputs["A"])
    links.new(upper, pick.inputs["B"])
    return pick.outputs["Result"]


def card_color(tree):
    noise = node(tree, "ShaderNodeTexNoise", (-1100, 300))
    noise.inputs["Scale"].default_value = 6.0
    ramp = node(tree, "ShaderNodeValToRGB", (-850, 300))
    ramp.color_ramp.elements[0].color = (0.05, 0.35, 0.04, 1)
    ramp.color_ramp.elements[1].color = (0.6, 0.85, 0.1, 1)
    tree.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    return ramp.outputs["Color"]


def card_alpha(tree):
    """Hard alpha cut-outs: Voronoi cells with holes, a binary mask."""
    voronoi = node(tree, "ShaderNodeTexVoronoi", (-1100, -100), feature="DISTANCE_TO_EDGE")
    voronoi.inputs["Scale"].default_value = 5.0
    mask = node(tree, "ShaderNodeMath", (-850, -100), operation="LESS_THAN")
    mask.inputs[1].default_value = 0.12
    tree.links.new(voronoi.outputs["Distance"], mask.inputs[0])
    return mask.outputs[0]


def add_mesh(kind, name, location, size, mat, rotation=(0, 0, 0)):
    if kind == "plane":
        bpy.ops.mesh.primitive_plane_add(size=size, location=location, rotation=rotation)
    elif kind == "cube":
        bpy.ops.mesh.primitive_cube_add(size=size, location=location, rotation=rotation)
    elif kind == "ico":
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=size, location=location)
    elif kind == "cylinder":
        bpy.ops.mesh.primitive_cylinder_add(radius=size, depth=4.0, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.append(mat)
    return obj


def build_scene(width, height, frames):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.preferences.edit.keyframe_new_interpolation_type = "LINEAR"
    rng = random.Random(SEED)
    scene = bpy.context.scene
    scene.name = "BloomBench"
    render = scene.render
    render.resolution_x, render.resolution_y = width, height
    render.resolution_percentage = 100
    render.pixel_aspect_x = render.pixel_aspect_y = 1.0
    render.fps, render.fps_base = FPS, 1.0
    scene.frame_start, scene.frame_end = 1, frames
    render.film_transparent = True
    render.use_persistent_data = True
    # No burn-in and no metadata that varies per run or names a machine or path.
    render.use_stamp = False
    for prop in dir(render):
        if prop.startswith("use_stamp_") and prop not in ("use_stamp_labels",):
            try:
                setattr(render, prop, False)
            except (AttributeError, TypeError):
                pass
    scene.view_settings.view_transform = RENDER_SETTINGS["viewTransform"]
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.display_settings.display_device = "sRGB"
    world = bpy.data.worlds.new("BenchWorld")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.05, 0.07, 0.1, 1)
    scene.world = world

    grain = grain_group(width, height)
    ground = material("Ground", grain, build_color=ground_color, roughness=0.65)
    add_mesh("plane", "Ground", (0, 0, 0), 60.0, ground)
    wall = material("ChirpWall", grain, build_color=chirp_color, roughness=0.8)
    add_mesh("plane", "ChirpWall", (0, 9, 3.2), 1.0, wall, rotation=(math.pi / 2, 0, 0))
    bpy.context.active_object.scale = (16, 6.4, 1)

    diffuse = [material(f"Hue{i}", grain, color=h, roughness=0.35) for i, h in enumerate(HUES)]
    glowing = [material(f"Glow{i}", grain, color=h, emission=3.0) for i, h in enumerate(HUES)]
    for index in range(12):
        angle = math.radians(-60 + index * 11)
        add_mesh("cylinder", f"Pillar{index}", (10 * math.sin(angle), 6 + 3 * math.cos(angle), 2),
                 0.22, diffuse[index % len(diffuse)])
    for index in range(360):
        kind = "ico" if rng.random() < 0.6 else "cube"
        location = (rng.uniform(-8, 8), rng.uniform(-4, 6), rng.uniform(0.2, 4))
        size = rng.uniform(0.04, 0.12)
        mats = glowing if rng.random() < 0.25 else diffuse
        obj = add_mesh(kind, f"Bit{index:03d}", location, size * (2 if kind == "cube" else 1),
                       mats[rng.randrange(len(mats))])
        if index % 6 == 0:
            spin = rng.uniform(1.5, 4.0)
            keyed(obj, "rotation_euler", [(1, (0, 0, 0)), (240, (spin, spin * 0.5, spin * 1.3))])
            linear_fcurves(obj)

    cards = material("Card", grain, build_color=card_color, roughness=0.5, build_alpha=card_alpha)
    for index in range(5):
        x = -6 + index * 3
        obj = add_mesh("plane", f"Card{index}", (x, -3 + rng.uniform(-1, 1), 1.6), 1.0, cards,
                       rotation=(math.pi / 2, 0, rng.uniform(-0.4, 0.4)))
        obj.scale = (2.0, 3.0, 1.0)
        if index % 2 == 0:
            keyed(obj, "location", [(1, obj.location.copy()),
                                    (240, (x + 2.5, obj.location.y, obj.location.z))])
            linear_fcurves(obj)
    veil = material("Veil", grain, color=(0.9, 0.5, 0.2), roughness=0.3, alpha=0.45)
    add_mesh("plane", "Veil", (3.5, 1.5, 2.5), 1.0, veil, rotation=(math.pi / 2, 0, 0.3))
    bpy.context.active_object.scale = (2.5, 1.8, 1)

    bpy.ops.object.light_add(type="SUN", location=(0, 0, 10), rotation=(0.7, 0.2, 0.9))
    bpy.context.active_object.data.energy = 3.0
    bpy.context.active_object.data.angle = math.radians(2.0)
    bpy.ops.object.light_add(type="AREA", location=(-4, -6, 6), rotation=(0.8, 0, -0.5))
    bpy.context.active_object.data.energy = 600.0
    bpy.context.active_object.data.size = 4.0

    bpy.ops.object.empty_add(location=(-1, 2, 1.2))
    target = bpy.context.active_object
    target.name = "CameraTarget"
    keyed(target, "location", [(1, (-1, 2, 1.2)), (240, (1.5, 3, 1.0))])
    linear_fcurves(target)
    bpy.ops.object.camera_add(location=(-7, -14, 3.2))
    camera = bpy.context.active_object
    camera.data.lens = 32.0
    camera.data.sensor_width = 36.0
    keyed(camera, "location", [(1, (-7, -14, 3.2)), (240, (7, -10, 2.0))])
    linear_fcurves(camera)
    track = camera.constraints.new("TRACK_TO")
    track.target = target
    scene.camera = camera
    return scene


def configure_cycles(scene, device, samples):
    scene.render.engine = "CYCLES"
    cycles = scene.cycles
    if device == "CPU":
        cycles.device = "CPU"
    else:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = device
        prefs.get_devices()
        enabled = 0
        for dev in prefs.devices:
            dev.use = dev.type == device
            enabled += dev.use
        if not enabled:
            raise SystemExit(f"no {device} device available")
        cycles.device = "GPU"
    cycles.samples = samples or RENDER_SETTINGS["samples"]
    cycles.use_adaptive_sampling = True
    cycles.adaptive_threshold = RENDER_SETTINGS["adaptiveThreshold"]
    cycles.seed = SEED
    cycles.use_animated_seed = False
    cycles.use_denoising = True
    cycles.denoiser = "OPENIMAGEDENOISE"
    cycles.denoising_use_gpu = device != "CPU"
    cycles.max_bounces = RENDER_SETTINGS["maxBounces"]
    cycles.use_auto_tile = False
    layer = scene.view_layers[0]
    layer.use_pass_z = True
    layer.use_pass_normal = True
    layer.use_pass_cryptomatte_object = True
    layer.pass_cryptomatte_depth = 4
    for name, kind in (("bench_color", "COLOR"), ("bench_id", "VALUE")):
        aov = layer.aovs.add()
        aov.name, aov.type = name, kind


def configure_outputs(scene, tier, out_root, formats):
    base = f"{out_root.rstrip('/')}/{tier}/"
    render = scene.render
    settings = render.image_settings
    if "exr_multilayer" in formats:
        settings.file_format = "OPEN_EXR_MULTILAYER"
        settings.color_depth = "16"
        settings.exr_codec = "ZIP"
        render.filepath = base + "exr_multilayer/" + SEQUENCE_PATTERN["exr_multilayer"].format(
            tier=tier)[:-len(".exr")]
        render.use_file_extension = True
    flat = [name for name in formats if name in FLAT_FORMATS]
    scene.use_nodes = True
    render.use_compositing = True
    tree = scene.node_tree
    tree.nodes.clear()
    layers = tree.nodes.new("CompositorNodeRLayers")
    composite = tree.nodes.new("CompositorNodeComposite")
    tree.links.new(layers.outputs["Image"], composite.inputs["Image"])
    if flat:
        output = tree.nodes.new("CompositorNodeOutputFile")
        output.base_path = base
        # The node format starts as the scene's (multilayer EXR would turn the slots into
        # layers of one file); per-slot formats need a single-layer node format.
        output.format.file_format = "PNG"
        output.file_slots.clear()
        for name in flat:
            file_format, depth, codec = FLAT_FORMATS[name]
            stem = SEQUENCE_PATTERN[name].format(tier=tier).rsplit(".", 1)[0]
            output.file_slots.new(f"{name}/{stem}")
            slot = output.file_slots[-1]
            slot.use_node_format = False
            slot.format.file_format = file_format
            slot.format.color_mode = "RGBA"
            slot.format.color_depth = depth
            if codec:
                slot.format.exr_codec = codec
            else:
                slot.format.compression = 15
            tree.links.new(layers.outputs["Image"], output.inputs[-1])
    if "exr_multilayer" not in formats:
        # The render output itself must go somewhere; keep it out of the media tree.
        settings.file_format = "PNG"
        render.filepath = f"{base}.discard/frame.####"


def main():
    args = parse_args()
    width, height, frames, tier_formats, _ = TIERS[args.tier]
    formats = tier_formats
    if args.formats:
        formats = tuple(name for name in args.formats.split(",") if name)
        unknown = set(formats) - set(tier_formats)
        if unknown:
            raise SystemExit(f"formats not in tier {args.tier}: {sorted(unknown)}")
    if args.frames:
        frames = min(frames, args.frames)
    scene = build_scene(width, height, TIERS[args.tier][2])
    scene.frame_end = frames
    if args.only_frame:
        scene.frame_start = scene.frame_end = args.only_frame
    configure_cycles(scene, args.device, args.samples)
    configure_outputs(scene, args.tier, args.out, formats)
    print(f"BENCH_SCENE tier={args.tier} size={width}x{height} frames={frames} "
          f"formats={','.join(formats)} blender={bpy.app.version_string}", flush=True)
    bpy.ops.render.render(animation=True)
    print("BENCH_SCENE done", flush=True)


main()
