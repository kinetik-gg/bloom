#!/usr/bin/env python3
"""Generate, encode, catalogue and verify Bloom's benchmark media.

The media itself never enters the repository. This tool writes it below a media root (argument
--root, else $BLOOM_BENCH_MEDIA, else ./bench-media) and keeps tools/bench/media_manifest.json,
which pins the generator, records every asset's geometry and format, and a SHA-256 per asset.

    bench_media.py render   --tier 720p [--blender blender] [--device OPTIX]   # Blender, Cycles
    bench_media.py encode   --tier 720p                                     # FFmpeg from PNG16
    bench_media.py chart    [--tier 1080p]                                  # procedural chart
    bench_media.py manifest --provenance "Blender 4.5.x LTS, Cycles OptiX, <GPU>"
    bench_media.py verify   [--json report.json]

Verification statuses per asset:
- verified-exact: the SHA-256 matches the manifest (this is the stored media);
- regenerated-compatible: checksums differ but frame count, numbering, dimensions and format
  (PNG depth/channels, EXR compression/channel list, video codec/profile/pixel format/tags/frame
  count) all match: media regenerated on another machine, acceptable for benchmarking;
- incompatible / missing: not usable.
An asset's SHA-256 is the SHA-256 of its sha256sum-style listing ("<hex>  <file name>\\n" per file,
sorted by name); the per-file listing is kept in <root>/.checksums/.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_spec as spec  # noqa: E402
from imageio_min import read_exr_header, read_png_header  # noqa: E402

MANIFEST = HERE / "media_manifest.json"
MANIFEST_VERSION = 1
# The encoders take colour tags from the filtered frames; -color_* options alone are dropped.
SETPARAMS = ("setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv:"
             "chroma_location=left")


def media_root(value):
    return Path(value or os.environ.get(spec.MEDIA_ROOT_ENV) or spec.DEFAULT_MEDIA_ROOT)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(8 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def hash_files(paths, jobs=8):
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        return dict(zip(paths, pool.map(sha256_file, paths)))


def listing_digest(digests):
    lines = "".join(f"{digests[path]}  {path.name}\n" for path in sorted(digests, key=lambda p: p.name))
    return hashlib.sha256(lines.encode()).hexdigest(), lines


def sequence_files(root, relative_pattern):
    directory = (root / relative_pattern).parent
    prefix, suffix = Path(relative_pattern).name.split("####")
    files = []
    if directory.is_dir():
        for entry in directory.iterdir():
            name = entry.name
            if name.startswith(prefix) and name.endswith(suffix):
                number = name[len(prefix):len(name) - len(suffix)]
                if number.isdigit():
                    files.append((int(number), entry))
    return sorted(files)


def image_structure(path):
    if path.suffix == ".png":
        header = read_png_header(path)
        return {"width": header["width"], "height": header["height"],
                "png": {"bitDepth": header["bitDepth"], "channels": header["channels"]}}
    header = read_exr_header(path)
    return {"width": header["width"], "height": header["height"],
            "exr": {"compression": header["compression"],
                    "channels": [list(c) for c in sorted(header["channels"])]}}


def probe_video(path):
    command = ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_packets",
               "-show_entries", "stream=codec_name,profile,pix_fmt,width,height,r_frame_rate,"
               "nb_read_packets,color_primaries,color_transfer,color_space,color_range,"
               "chroma_location", "-of", "json", str(path)]
    stream = json.loads(subprocess.run(command, check=True, capture_output=True,
                                       text=True).stdout)["streams"][0]
    return {"width": stream["width"], "height": stream["height"],
            "frames": int(stream["nb_read_packets"]),
            "video": {key: stream.get(key) for key in (
                "codec_name", "profile", "pix_fmt", "r_frame_rate", "color_primaries",
                "color_transfer", "color_space", "color_range", "chroma_location")}}


def describe_sequence(root, relative_pattern, jobs):
    files = sequence_files(root, relative_pattern)
    if not files:
        return None
    structures = {json.dumps(image_structure(path), sort_keys=True) for _, path in files}
    if len(structures) != 1:
        raise SystemExit(f"{relative_pattern}: members differ in geometry/format")
    digests = hash_files([path for _, path in files], jobs)
    digest, lines = listing_digest(digests)
    numbers = [number for number, _ in files]
    entry = {"type": "sequence", "path": relative_pattern, "first": numbers[0],
             "frames": len(numbers), "contiguous": numbers == list(range(numbers[0],
                                                                         numbers[-1] + 1)),
             "fps": spec.FPS, **json.loads(structures.pop()),
             "files": len(files), "bytes": sum(path.stat().st_size for _, path in files),
             "sha256": digest}
    return entry, lines


def describe_video(root, relative_path):
    path = root / relative_path
    if not path.is_file():
        return None
    digest = sha256_file(path)
    entry = {"type": "video", "path": relative_path, "fps": spec.FPS, **probe_video(path),
             "files": 1, "bytes": path.stat().st_size, "sha256": digest}
    return entry, f"{digest}  {path.name}\n"


def footage_entries(root, tiers):
    for tier, fmt, kind in spec.footage_assets():
        if tier in tiers:
            relative = (spec.sequence_path(tier, fmt) if kind == "sequence"
                        else spec.video_path(tier, fmt))
            yield f"{tier}/{fmt}", "footage", tier, fmt, kind, relative


def chart_entries(tiers):
    import chart
    for tier in chart.TIERS:
        if tier not in tiers:
            continue
        for fmt in ("png16", "exr_float_zip"):
            ext = "png" if fmt == "png16" else "exr"
            yield (f"chart/{tier}/{fmt}", "chart", tier, fmt, "sequence",
                   f"chart/{tier}/{fmt}/chart_{tier}_{fmt}.####.{ext}")
        for variant, values in chart.VIDEO_VARIANTS.items():
            if variant not in chart.VIDEO_SKIP.get(tier, set()):
                yield (f"chart/{tier}/{variant}", "chart", tier, variant, "video",
                       f"chart/{tier}/video/chart_{tier}_{variant}.{values[6]}")


def all_entries(tiers):
    yield from footage_entries(None, tiers)
    yield from chart_entries(tiers)

# --- commands -----------------------------------------------------------------------------------


def command_render(args):
    root = media_root(args.root).resolve()
    command = [args.blender, "-b", "--factory-startup", "-P", str(HERE / "blender_scene.py"), "--",
               "--tier", args.tier, "--out", str(root), "--device", args.device]
    print(" ".join(command), flush=True)
    subprocess.run(command, check=True)


def command_encode(args):
    root = media_root(args.root)
    tiers = args.tier or list(spec.TIER_ORDER)
    for tier in tiers:
        _, _, frames, _, videos = spec.TIERS[tier]
        master = root / spec.sequence_path(tier, "png16").replace("####", "%04d")
        if not (root / spec.sequence_path(tier, "png16").replace("####", "0001")).is_file():
            raise SystemExit(f"{tier}: the 16-bit PNG master is missing")
        for fmt in args.format or videos:
            if fmt not in videos:
                continue
            _, alpha, pix_fmt, encoder = spec.VIDEO_ENCODES[fmt]
            out = root / spec.video_path(tier, fmt)
            out.parent.mkdir(parents=True, exist_ok=True)
            matrix = "scale=out_color_matrix=bt709:out_range=tv"
            chain = matrix if alpha else f"premultiply=inplace=1,{matrix}"
            command = ["nice", "-n", "10", "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                       "-framerate", str(spec.FPS), "-start_number", "1", "-i", str(master),
                       "-frames:v", str(frames), "-vf", f"{chain},format={pix_fmt},{SETPARAMS}",
                       "-pix_fmt", pix_fmt, *spec.VIDEO_TAGS, *encoder, "-an", str(out)]
            print(f"encode {tier}/{fmt}", flush=True)
            subprocess.run(command, check=True, env={**os.environ, "SVT_LOG": "1"})


def command_chart(args):
    root = media_root(args.root)
    command = [sys.executable, str(HERE / "chart.py"), "--out", str(root)]
    if args.tier:
        command += ["--tiers", ",".join(args.tier)]
    subprocess.run(command, check=True)


def load_manifest(path):
    if path.is_file():
        return json.loads(path.read_text())
    return None


def ffmpeg_version():
    try:
        return subprocess.run(["ffmpeg", "-version"], capture_output=True, text=True,
                              check=True).stdout.splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        return None


def command_manifest(args):
    root = media_root(args.root)
    path = Path(args.manifest)
    manifest = load_manifest(path) or {
        "manifestVersion": MANIFEST_VERSION,
        "mediaRootDefault": {"env": spec.MEDIA_ROOT_ENV, "relative": spec.DEFAULT_MEDIA_ROOT},
        # Reserved for a future object-storage mirror of the media. Unused: nothing is uploaded.
        "remoteObjectStorageUrl": None,
        "assets": {}}
    manifest["generator"] = {
        "scene": "tools/bench/blender_scene.py", "blenderSeries": f"{spec.BLENDER_SERIES} LTS",
        "renderSettings": spec.RENDER_SETTINGS, "tiers": {
            tier: {"width": w, "height": h, "frames": n, "images": list(i), "videos": list(v)}
            for tier, (w, h, n, i, v) in spec.TIERS.items()},
        "videoEncodes": {fmt: {"file": f, "alpha": a, "pixFmt": p, "args": e,
                               "tags": spec.VIDEO_TAGS}
                         for fmt, (f, a, p, e) in spec.VIDEO_ENCODES.items()},
        "chart": "tools/bench/chart.py (procedural; truth in tools/bench/chart_truth.json)"}
    if args.provenance:
        manifest.setdefault("provenance", {})["render"] = args.provenance
    encoder = ffmpeg_version()
    if encoder:
        manifest.setdefault("provenance", {})["encoder"] = encoder
    tiers = set(args.tier or spec.TIER_ORDER)
    checksums = root / ".checksums"
    checksums.mkdir(parents=True, exist_ok=True)
    for asset_id, kind, tier, fmt, asset_type, relative in all_entries(tiers):
        described = (describe_sequence(root, relative, args.jobs) if asset_type == "sequence"
                     else describe_video(root, relative))
        if described is None:
            print(f"absent   {asset_id}")
            continue
        entry, lines = described
        entry.update({"kind": kind, "tier": tier, "format": fmt})
        manifest["assets"][asset_id] = entry
        (checksums / (asset_id.replace("/", "__") + ".sha256")).write_text(lines)
        print(f"recorded {asset_id} files={entry['files']} bytes={entry['bytes']}", flush=True)
    ordered = dict(sorted(manifest["assets"].items(), key=lambda item: asset_sort_key(item[1])))
    manifest["assets"] = ordered
    manifest["totals"] = {"assets": len(ordered),
                          "bytes": sum(a["bytes"] for a in ordered.values())}
    path.write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"manifest {path}: {len(ordered)} assets, {manifest['totals']['bytes'] / 1e9:.1f} GB")


def asset_sort_key(entry):
    order = list(spec.TIER_ORDER)
    return (entry["kind"] != "footage", order.index(entry["tier"]), entry["type"], entry["format"])


STRUCTURE_KEYS = ("type", "width", "height", "frames", "first", "contiguous", "png", "exr",
                  "video", "fps")


def verify_asset(root, entry, jobs):
    relative = entry["path"]
    if entry["type"] == "sequence":
        described = describe_sequence(root, relative, jobs)
    else:
        described = describe_video(root, relative)
    if described is None:
        return {"status": "missing"}
    actual, lines = described
    if actual["sha256"] == entry["sha256"]:
        return {"status": "verified-exact"}
    differences = {key: {"manifest": entry.get(key), "actual": actual.get(key)}
                   for key in STRUCTURE_KEYS if entry.get(key) != actual.get(key)}
    if differences:
        return {"status": "incompatible", "differences": differences}
    return {"status": "regenerated-compatible", "sha256": actual["sha256"]}


def verify(root, manifest, select=None, jobs=8, progress=True):
    """Shared with run_bench.py. Returns {"status": overall, "assets": {id: result}}."""
    results = {}
    for asset_id, entry in manifest["assets"].items():
        if select and not select(asset_id, entry):
            continue
        results[asset_id] = verify_asset(root, entry, jobs)
        if progress:
            print(f"{results[asset_id]['status']:<23} {asset_id}", flush=True)
    statuses = {result["status"] for result in results.values()}
    overall = ("verified-exact" if statuses <= {"verified-exact"} else
               "regenerated-compatible" if statuses <= {"verified-exact",
                                                        "regenerated-compatible"} else "failed")
    return {"status": overall, "assets": results}


def command_verify(args):
    root = media_root(args.root)
    manifest = load_manifest(Path(args.manifest))
    if manifest is None:
        raise SystemExit("no manifest")
    tiers = set(args.tier) if args.tier else None
    report = verify(root, manifest, (lambda _, e: e["tier"] in tiers) if tiers else None,
                    args.jobs)
    print(f"overall: {report['status']}")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + "\n")
    return 0 if report["status"] != "failed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", help=f"media root (default ${spec.MEDIA_ROOT_ENV} or "
                                        f"./{spec.DEFAULT_MEDIA_ROOT})")
    parser.add_argument("--manifest", default=str(MANIFEST))
    parser.add_argument("--jobs", type=int, default=8, help="parallel hashing threads")
    sub = parser.add_subparsers(dest="command", required=True)
    render = sub.add_parser("render", help="render one tier with Blender (Cycles)")
    render.add_argument("--tier", required=True, choices=spec.TIER_ORDER)
    render.add_argument("--blender", default=shutil.which("blender") or "blender")
    render.add_argument("--device", default="OPTIX")
    encode = sub.add_parser("encode", help="encode video from the 16-bit PNG master")
    encode.add_argument("--tier", action="append", choices=spec.TIER_ORDER)
    encode.add_argument("--format", action="append", choices=spec.VIDEO_FORMATS)
    chart = sub.add_parser("chart", help="generate the colour/format chart")
    chart.add_argument("--tier", action="append", choices=("1080p", "2160p", "4320p"))
    manifest = sub.add_parser("manifest", help="record assets present under the root")
    manifest.add_argument("--tier", action="append", choices=spec.TIER_ORDER)
    manifest.add_argument("--provenance", help="render machine summary, no host or user names")
    check = sub.add_parser("verify", help="verify the media against the manifest")
    check.add_argument("--tier", action="append", choices=spec.TIER_ORDER)
    check.add_argument("--json", help="write the verification report here")
    args = parser.parse_args()
    handler = {"render": command_render, "encode": command_encode, "chart": command_chart,
               "manifest": command_manifest, "verify": command_verify}[args.command]
    sys.exit(handler(args) or 0)


if __name__ == "__main__":
    main()
