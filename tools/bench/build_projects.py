"""Build Bloom benchmark projects headlessly. Runs inside Bloom: `bloom-cli run build_projects.py`.

bloom-cli passes no script arguments, so the job list comes from the JSON file named by
$BLOOM_BENCH_BUILD (run_bench.py writes it):

    [{"project": "/abs/out/720p_png16_A.bloom", "variant": "A" | "B",
      "media": "/abs/media/720p/png16" (sequence folder) or "/abs/.../file.mov",
      "label": "720p png16", "width": 1280, "height": 720}, ...]

Variant A is the footage alone (decode-bound); a job may also list its own "exports". Variant B
is a typical shot (compute-bound): a background solid, the footage blurred and scaled behind, two
transformed footage layers, one of them through a track matte (an alpha matte solid), and an
animated text layer. Both carry three export jobs: ProRes 422 HQ MOV, H.264 MP4 and an OpenEXR
sequence, written below exports/<project name>/ next to the project, replacing earlier runs and
evaluating every frame.

Variant L<n> (for example L25) is a stack of n layers cycling through the footage and the media
listed in the job's "mix" (see build_l); the job also carries "layers": n. An optional
"workAreaFrames" shortens the work area, so RAM preview fills (and playback loops) only those
frames.
"""
import json
import os

import _bloom
import bloom
from bloom import _state


DEFAULT_EXPORTS = (("prores422hq", "prores-mov", "hq", "{name}.mov"),
                   ("h264", "h264-mp4", "high", "{name}.mp4"),
                   ("exr", "exr", "", "exr/{name}.exr"))


def outputs(result, name):
    return [entry["id"] for entry in result["outputs"] if entry["name"] == name]


def output(result, name):
    found = outputs(result, name)
    if not found:
        raise RuntimeError(f"operation produced no {name!r}: {result}")
    return found[0]


def export_output(fmt, profile, path, compression=""):
    settings = {"format": fmt, "codecProfile": profile, "compression": compression,
                "outputColorSpace": "", "display": "", "view": "", "bakeLook": True,
                "includeAudio": False, "useCachedFrames": False, "audioSampleRate": 48000,
                "encoder": "automatic",
                "naming": {"framePadding": 4, "pattern": "<base>.####.<ext>"}}
    return json.dumps({"preset": "", "settings": settings,
                       "destination": {"portability": "project-relative", "path": path,
                                       "overwrite": "replace"},
                       "enabled": True, "importIntoAssets": False})


def set_param(node, role, **value):
    bloom.ops.node.set_parameter_value(node=node, role=role, **value)


def composition_record(composition):
    for record in bloom.data.snapshot()["compositions"]:
        if record["id"] == composition:
            return record
    raise RuntimeError(f"composition {composition} not found")


def node_ids(composition, type_id):
    return {node["id"] for node in composition_record(composition)["nodes"]
            if node["type_id"] == type_id}


def add_footage_layer(composition, asset):
    """Adds the footage as a new top layer; returns (layer id, its Layer Output node id)."""
    before = node_ids(composition, "bloom.layer-output")
    result = bloom.ops.layer.add_from_assets(composition=composition, assets=(asset,))
    added = node_ids(composition, "bloom.layer-output") - before
    if len(added) != 1:
        raise RuntimeError("could not identify the new layer's output node")
    return output(result, "layer"), added.pop()


def build_b(composition, asset, width, height, base_layer, base_node, image_node, label):
    """Turn the single-footage composition into a typical shot."""
    unit = width / 1920.0
    # The footage already in the composition becomes the blurred, slightly enlarged background.
    blur = bloom.ops.node.add_fed_by(composition=composition, nodeType="bloom.gaussian-blur",
                                     node=image_node, port="image")
    blur_node = output(blur, "node")
    bloom.ops.node.connect(composition=composition, sourceNode=blur_node, sourcePort="image",
                           targetNode=base_node, port="image")
    set_param(blur_node, "radius", number=12.0 * unit)
    set_param(base_node, "scale", vector=(1.12, 1.12))
    # A solid under everything.
    solid = bloom.ops.layer.add_solid(composition=composition, name="Background",
                                      color=(0.04, 0.05, 0.09, 1.0))
    # Without `before` the slot moves to the bottom of the stack.
    bloom.ops.layer.move_before(composition=composition, slot=output(solid, "slot"))
    # Two transformed copies of the footage on top.
    mid_layer, mid_node = add_footage_layer(composition, asset)
    set_param(mid_node, "scale", vector=(0.62, 0.62))
    set_param(mid_node, "rotation", number=6.0)
    set_param(mid_node, "position", vector=(width * 0.42, height * 0.46))
    set_param(mid_node, "opacity", number=0.92)
    top_layer, top_node = add_footage_layer(composition, asset)
    set_param(top_node, "scale", vector=(0.36, 0.36))
    set_param(top_node, "rotation", number=-11.0)
    set_param(top_node, "position", vector=(width * 0.74, height * 0.3))
    # A track matte: an alpha matte solid above the top footage layer.
    matte = bloom.ops.layer.add_solid(composition=composition, name="Matte",
                                      color=(1.0, 1.0, 1.0, 1.0),
                                      position=(width * 0.74, height * 0.3))
    matte_layer = output(matte, "layer")
    matte_node = output(matte, "layerOutputNode")
    set_param(matte_node, "scale", vector=(0.22, 0.3))
    set_param(matte_node, "rotation", number=20.0)
    bloom.ops.layer.set_track_matte(composition=composition, layer=top_layer,
                                    source=matte_layer, channel=0, inverted=False)
    # Animated text.
    text = bloom.ops.layer.add_text(composition=composition, name="Title",
                                    text=f"BLOOM BENCH {label}", size=90.0 * unit,
                                    color=(1.0, 0.95, 0.85, 1.0),
                                    position=(width * 0.5, height * 0.85))
    position = output(text, "positionParameter")
    with bloom.transactions.group("Slide the title"):
        bloom.ops.animation.create_for_parameter(position, composition=composition, time=0)
        bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
            position, 0, width * 0.35, composition=composition, time=0)
        bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
            position, 0, width * 0.65, composition=composition, time=47)
    return {"blurNode": blur_node, "layers": [base_layer, mid_layer, top_layer, matte_layer]}


BLENDS = (0, 3, 1, 2, 4, 6)  # Normal, Screen, Add, Multiply, Overlay, Lighten


def build_l(composition, job, primary, base_node, image_node, directory):
    """Turn the single-footage composition into a stack of job["layers"] layers (variant L<n>).

    The footage cycles through the primary medium and job["mix"] (other formats and tiers), every
    fifth layer is a small solid, each layer has its own scale, rotation, position, opacity and
    blend mode, the base footage is blurred, a background solid sits under everything and an
    animated text layer on top. The count includes the background, the base and the text.
    """
    width, height, count = job["width"], job["height"], int(job["layers"])
    assets = [primary]
    for path in job.get("mix") or ():
        imported = bloom.ops.asset.import_(paths=(path,), projectDirectory=directory)
        assets.append(output(imported, "asset"))
    blur = bloom.ops.node.add_fed_by(composition=composition, nodeType="bloom.gaussian-blur",
                                     node=image_node, port="image")
    blur_node = output(blur, "node")
    bloom.ops.node.connect(composition=composition, sourceNode=blur_node, sourcePort="image",
                           targetNode=base_node, port="image")
    set_param(blur_node, "radius", number=8.0 * width / 1920.0)
    solid = bloom.ops.layer.add_solid(composition=composition, name="Background",
                                      color=(0.04, 0.05, 0.09, 1.0))
    bloom.ops.layer.move_before(composition=composition, slot=output(solid, "slot"))
    columns = 5
    for index in range(max(0, count - 3)):
        cell_x = (index % columns + 0.5) / columns
        cell_y = ((index // columns) % 4 + 0.5) / 4
        if index % 5 == 4:
            added = bloom.ops.layer.add_solid(
                composition=composition, name=f"Solid {index + 1}",
                color=((index * 37) % 100 / 100.0, (index * 61) % 100 / 100.0, 0.6, 1.0),
                position=(width * cell_x, height * cell_y))
            node = output(added, "layerOutputNode")
            set_param(node, "scale", vector=(0.12, 0.1))
        else:
            _, node = add_footage_layer(composition, assets[index % len(assets)])
            set_param(node, "scale", vector=(0.3 + 0.05 * (index % 4),) * 2)
            set_param(node, "position", vector=(width * cell_x, height * cell_y))
        set_param(node, "rotation", number=float((index * 7) % 30 - 15))
        set_param(node, "opacity", number=0.75 + 0.05 * (index % 5))
        set_param(node, "blendMode", integer=BLENDS[index % len(BLENDS)])
    text = bloom.ops.layer.add_text(composition=composition, name="Title",
                                    text=f"BLOOM {count} LAYERS", size=90.0 * width / 1920.0,
                                    color=(1.0, 0.95, 0.85, 1.0),
                                    position=(width * 0.5, height * 0.85))
    position = output(text, "positionParameter")
    with bloom.transactions.group("Slide the title"):
        bloom.ops.animation.create_for_parameter(position, composition=composition, time=0)
        bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
            position, 0, width * 0.35, composition=composition, time=0)
        bloom.ops.animation.set_keyframe_at_time_for_parameter_component(
            position, 0, width * 0.65, composition=composition, time=47)
    return {"blurNode": blur_node, "layerCount": count}


def build(job):
    project = os.path.abspath(job["project"])
    directory = os.path.dirname(project)
    os.makedirs(directory, exist_ok=True)
    name = os.path.splitext(os.path.basename(project))[0]
    host = _bloom.Host("")
    token = _state.bind(host)
    try:
        initial = [c.id for c in bloom.data.compositions]
        imported = bloom.ops.asset.import_(paths=(job["media"],), projectDirectory=directory)
        asset = output(imported, "asset")
        created = bloom.ops.composition.add_from_assets(assets=(asset,))
        composition = output(created, "composition")
        bloom.ops.composition.set_name(composition=composition, name=name)
        for stale in initial:
            bloom.ops.composition.delete(composition=stale)
        detail = {}
        if job["variant"] == "B":
            detail = build_b(composition, asset, job["width"], job["height"],
                             output(created, "layer"), output(created, "layerOutputNode"),
                             output(created, "imageNode"), job["label"])
        elif job["variant"].startswith("L"):
            detail = build_l(composition, job, asset, output(created, "layerOutputNode"),
                             output(created, "imageNode"), directory)
        if job.get("workAreaFrames"):
            # A shorter work area: RAM preview fills (and playback loops) only these frames.
            bloom.ops.composition.set_work_area(0, int(job["workAreaFrames"]),
                                                composition=composition)
        exports = f"exports/{name}"
        jobs = {}
        # A job may name its own exports: [[key, format, codec profile, file name], ...].
        for key, fmt, profile, file_name in job.get("exports") or DEFAULT_EXPORTS:
            path = f"{exports}/{file_name.format(name=name)}"
            compression = ""
            added = bloom.ops.export.add_job(composition=composition, name=key,
                                             range="composition",
                                             outputs=(export_output(fmt, profile, path,
                                                                    compression),))
            jobs[key] = output(added, "exportJob")
        host.save(project)
        return {"project": project, "composition": composition, "exportJobs": jobs, **detail}
    finally:
        _state._host.reset(token)


def main():
    jobs = json.loads(open(os.environ["BLOOM_BENCH_BUILD"], encoding="utf-8").read())
    results = []
    for job in jobs:
        try:
            results.append({"ok": True, **build(job)})
        except Exception as error:  # report every project, keep building the rest
            results.append({"ok": False, "project": job["project"], "error": str(error)})
        print("BENCH_BUILT " + json.dumps(results[-1]), flush=True)
    report = os.environ.get("BLOOM_BENCH_BUILD_REPORT")
    if report:
        with open(report, "w", encoding="utf-8") as handle:
            json.dump(results, handle, indent=1)


main()
