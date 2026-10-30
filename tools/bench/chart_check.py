#!/usr/bin/env python3
"""Colour/format accuracy checks with the procedural chart (run_bench.py chart ...).

For every chart asset of the selected tiers (PNG16, float EXR and each tagged video variant) it
imports the asset into a new Bloom project (headlessly, bloom-cli), renders frame 1 to a linear
Rec.709 float EXR and compares every patch with tools/bench/chart_truth.json:

- per patch: mean of the patch's sample rectangle, error in 8-bit sRGB code units against the
  truth (and relative error for the EXR-only values above 1.0);
- siting: the centroid of each tiny saturated square against its true centre, so a chroma shift
  (for example a vertical shift on import) shows up in pixels;
- export round trips from the PNG16 chart: ProRes 422 HQ, ProRes 4444, DNxHR HQX, OpenEXR and
  H.264 (when its encoder is installed). Each export's tags are probed, its frame is decoded with
  FFmpeg by those tags and compared with the truth, and the export is imported back into Bloom
  (the declared-sampling reopen check) and compared again.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import chart  # noqa: E402
from imageio_min import read_exr  # noqa: E402

TRUTH = HERE / "chart_truth.json"
# Tolerances in 8-bit sRGB code units (max channel error of a patch mean).
TOLERANCE = {"png16": 0.5, "exr_float_zip": 0.5, "video8": 3.0, "video10": 2.0}
HDR_RELATIVE_TOLERANCE = 1e-3
SITING_TOLERANCE_PX = 0.35
ROUND_TRIP_EXPORTS = (("prores422hq", "prores-mov", "hq", "{name}.mov"),
                      ("prores4444", "prores-mov", "4444", "{name}.mov"),
                      ("dnxhr_hqx", "dnxhr-mxf", "hqx", "{name}.mxf"),
                      ("h264", "h264-mp4", "high", "{name}.mp4"),
                      ("exr", "exr", "", "exr/{name}.exr"))


def srgb_codes(linear):
    return chart.srgb_encode(linear) * 255.0


def sample(planes, rect):
    x, y, w, h = rect
    return np.array([float(planes[c][y:y + h, x:x + w].mean()) for c in "RGBA"])


def tolerance_for(source):
    if source in TOLERANCE:
        return TOLERANCE[source]
    pix_fmt = chart.VIDEO_VARIANTS[source][1]
    return TOLERANCE["video10" if "10" in pix_fmt else "video8"]


def expected_linear(patch, source):
    """Expected straight linear RGB and alpha for a patch read from `source`."""
    alpha = patch["alpha"]
    if source == "exr_float_zip":
        return np.array(patch["linear"]), alpha
    if source in chart.VIDEO_VARIANTS:
        kind = chart.VIDEO_VARIANTS[source][7]
        if not source.startswith("prores_4444"):
            alpha = 1.0
        if kind != "sdr":
            # Report HDR variants against the source linear values (1.0 = 203 cd/m2).
            return np.clip(np.array(patch["linear"]), 0, None), alpha
    return chart.srgb_decode(np.clip(patch["signal"], 0, 1)), alpha


def compare_planes(planes, truth, source):
    """Per-patch errors and siting shifts for one decoded frame (premultiplied linear planes)."""
    rows, worst = [], 0.0
    tolerance = tolerance_for(source)
    for patch in truth["patches"]:
        measured = sample(planes, patch["sample"])
        linear, alpha = expected_linear(patch, source)
        straight = measured[:3] / measured[3] if measured[3] > 1e-6 else measured[:3]
        row = {"region": patch["region"], "name": patch["name"],
               "expectedLinear": [round(float(v), 6) for v in linear],
               "measuredLinear": [round(float(v), 6) for v in straight],
               "alphaError": round(float(measured[3] - alpha), 5)}
        if patch["region"] == "hdr" and source in ("exr_float_zip",):
            denominator = np.maximum(np.abs(linear), 1e-4)
            row["relativeError"] = round(float(np.max(np.abs(straight - linear) / denominator)), 6)
            row["pass"] = row["relativeError"] <= HDR_RELATIVE_TOLERANCE
        elif alpha <= 1e-6:
            row["pass"] = abs(row["alphaError"]) <= 1 / 255
        else:
            error = float(np.max(np.abs(srgb_codes(straight) - srgb_codes(linear))))
            row["codeError"] = round(error, 3)
            row["pass"] = error <= tolerance and abs(row["alphaError"]) <= 2 / 255
            worst = max(worst, error)
        rows.append(row)
    siting = siting_shifts(planes, truth)
    return {"tolerance": tolerance, "worstCodeError": round(worst, 3),
            "failedPatches": [r["name"] for r in rows if not r["pass"]], "patches": rows,
            "siting": siting}


def siting_shifts(planes, truth):
    field = chart.srgb_decode(np.array(truth["pixel"]["sitingField"]["signal"]))
    shifts = []
    for square in truth["pixel"]["sitingSquares"]:
        x, y, w, h = square["rect"]
        x0, y0 = x - 6, y - 6
        window = np.stack([planes[c][y0:y + h + 6, x0:x + w + 6] for c in "RGB"], axis=-1)
        weight = np.abs(window - field).sum(axis=-1)
        total = weight.sum()
        if total <= 0:
            continue
        yy, xx = np.mgrid[0:weight.shape[0], 0:weight.shape[1]]
        cx = float((weight * xx).sum() / total) + x0
        cy = float((weight * yy).sum() / total) + y0
        shifts.append({"colour": square["colour"], "size": w, "columnParity": square["columnParity"],
                       "rowParity": square["rowParity"],
                       "dx": round(cx - (x + (w - 1) / 2), 3), "dy": round(cy - (y + (h - 1) / 2), 3)})
    dx = max((abs(s["dx"]) for s in shifts), default=0.0)
    dy = max((abs(s["dy"]) for s in shifts), default=0.0)
    return {"maxAbsDx": dx, "maxAbsDy": dy,
            "pass": dx <= SITING_TOLERANCE_PX and dy <= SITING_TOLERANCE_PX, "squares": shifts}


def decode_with_ffmpeg(path, width, height):
    """Decode frame 1 to straight RGB(A) linear planes using the file's own tags."""
    probe = json.loads(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=codec_name,profile,pix_fmt,color_primaries,color_transfer,color_space,"
         "color_range,chroma_location", "-of", "json", str(path)],
        capture_output=True, text=True, check=True).stdout)["streams"][0]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-frames:v", "1",
                          "-f", "rawvideo", "-pix_fmt", "rgba64le", "-"],
                         capture_output=True, check=True).stdout
    pixels = np.frombuffer(raw, dtype="<u2").reshape(height, width, 4) / 65535.0
    alpha = pixels[..., 3]
    # Linearise by the declared transfer: sRGB, or BT.1886 (gamma 2.4) for bt709-tagged files.
    if probe.get("color_transfer") in ("bt709", "smpte170m", "bt470bg"):
        probe["linearisedWith"] = "BT.1886 gamma 2.4"
        linear = pixels[..., :3] ** 2.4
    else:
        probe["linearisedWith"] = "sRGB (IEC 61966-2-1)"
        linear = chart.srgb_decode(pixels[..., :3])
    planes = {c: linear[..., i] * alpha for i, c in enumerate("RGB")}
    planes["A"] = alpha
    return probe, planes


class Bloom:
    def __init__(self, runner_args):
        self.args = runner_args

    def run(self, command, timeout=1800):
        from run_bench import bench_env
        with self.args.lock:
            return subprocess.run(command, capture_output=True, text=True, env=bench_env(),
                                  timeout=timeout)

    def build(self, jobs, directory):
        from run_bench import bench_env
        directory.mkdir(parents=True, exist_ok=True)
        spec = directory / "chart-jobs.json"
        report = directory / "chart-build.json"
        spec.write_text(json.dumps(jobs, indent=1))
        env = bench_env()
        env.update({"BLOOM_BENCH_BUILD": str(spec), "BLOOM_BENCH_BUILD_REPORT": str(report)})
        subprocess.run([str(self.args.bloom_cli), "run", str(HERE / "build_projects.py")],
                       env=env, capture_output=True, text=True)
        return {Path(row["project"]).stem: row for row in json.loads(report.read_text())}

    def render_linear(self, project, out):
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        done = self.run([str(self.args.bloom_cli), "render", "--project", str(project),
                         "--frame", "0", "--preset", "FlatExrRgba32fLinRec709SceneV1",
                         "--out", str(out)])
        frame = out / "frame.exr"
        if done.returncode != 0 or not frame.is_file():
            return None, (done.stdout + done.stderr).strip().splitlines()[-3:]
        return read_exr(frame), None

    def export(self, project, job):
        done = self.run([str(self.args.bloom_cli), "export", "--project", str(project),
                         "--job", str(job), "--replace"])
        events = re.search(r'^(\{"lastSequence".*\})$', done.stdout, re.MULTILINE)
        summary = {"exitCode": done.returncode}
        unavailable = re.search(r"^error\[bloom-cli\.export[^\]]*\]: (.*)$",
                                done.stdout + done.stderr, re.MULTILINE)
        if unavailable:
            summary.update({"outcome": "unavailable", "reason": unavailable.group(1)})
        if events:
            for event in json.loads(events.group(1)).get("events", []):
                if event.get("type") in ("succeeded", "failed", "cancelled"):
                    summary["outcome"] = event["type"]
                    summary["reason"] = event.get("reason")
                    for out in event.get("outputs", []):
                        summary["provenance"] = out.get("provenance")
                        summary["first"] = out.get("artifacts", {}).get("first")
        return summary


def chart_assets(root, tiers):
    for tier in tiers:
        base = root / "chart" / tier
        for source in ("png16", "exr_float_zip"):
            if (base / source).is_dir():
                yield tier, source, base / source
        for variant, values in chart.VIDEO_VARIANTS.items():
            path = base / "video" / f"chart_{tier}_{variant}.{values[6]}"
            if path.is_file():
                yield tier, variant, path


def summary_line(tier, source, result):
    if "error" in result:
        return f"| {tier} | {source} | error: {result['error']} | | |"
    return (f"| {tier} | {source} | {result['worstCodeError']} (tol {result['tolerance']}) | "
            f"{len(result['failedPatches'])} | dx {result['siting']['maxAbsDx']} "
            f"dy {result['siting']['maxAbsDy']} |")


def run_chart(args, root, work, tiers):
    truth = json.loads(TRUTH.read_text())
    bloom = Bloom(args)
    projects = work / "chart-projects"
    assets = list(chart_assets(root, tiers))
    jobs = []
    for tier, source, path in assets:
        width, height = chart.TIERS[tier]
        name = f"chart_{tier}_{source}"
        job = {"project": str(projects / name / f"{name}.bloom"), "variant": "A",
               "media": str(path.resolve()), "label": name, "width": width, "height": height,
               "exports": [list(e) for e in ROUND_TRIP_EXPORTS] if source == "png16" else [
                   ["exr", "exr", "", "exr/{name}.exr"]]}
        jobs.append(job)
    built = bloom.build(jobs, projects) if jobs else {}
    results = {"imports": {}, "roundTrips": {}}
    lines = ["| tier | source | worst code error | failed patches | siting shift px |",
             "|---|---|---|---|---|"]
    for tier, source, path in assets:
        name = f"chart_{tier}_{source}"
        row = built.get(name)
        key = f"{tier}/{source}"
        if not row or not row.get("ok"):
            results["imports"][key] = {"error": (row or {}).get("error", "project build failed")}
            lines.append(summary_line(tier, source, results["imports"][key]))
            continue
        planes, error = bloom.render_linear(Path(row["project"]), work / "chart-renders" / name)
        result = ({"error": error} if planes is None else
                  compare_planes(planes, truth["tiers"][tier], source))
        results["imports"][key] = result
        lines.append(summary_line(tier, source, result))
        print(lines[-1], flush=True)
        if source == "png16":
            results["roundTrips"][tier] = round_trips(bloom, row, truth["tiers"][tier], tier, work)
    lines += ["", "| tier | export | outcome | tags | decoded worst code error | reopened worst |",
              "|---|---|---|---|---|---|"]
    for tier, trips in results["roundTrips"].items():
        for key, trip in trips.items():
            tags = trip.get("tags", {})
            lines.append(
                f"| {tier} | {key} | {trip.get('outcome')} {trip.get('reason') or ''} | "
                f"{tags.get('pix_fmt', '')} {tags.get('color_space', '')} "
                f"{tags.get('color_transfer', '')} {tags.get('color_range', '')} | "
                f"{trip.get('decoded', {}).get('worstCodeError', '')} | "
                f"{trip.get('reopened', {}).get('worstCodeError', '')} |")
    return results, "\n".join(lines)


def round_trips(bloom, row, truth, tier, work):
    project = Path(row["project"])
    width, height = truth["size"]
    trips = {}
    for key, *_ in ROUND_TRIP_EXPORTS:
        job = row["exportJobs"].get(key)
        if job is None:
            continue
        trip = bloom.export(project, job)
        trips[key] = trip
        first = trip.get("first")
        if trip.get("outcome") != "succeeded" or not first:
            continue
        exported = Path(first) if Path(first).is_absolute() else project.parent / first
        if not exported.exists():
            exported = project.parent / Path(first).name
        if key == "exr":
            trip["decoded"] = compare_planes(read_exr(exported), truth, "exr_float_zip")
        else:
            trip["tags"], planes = decode_with_ffmpeg(exported, width, height)
            source = "prores_4444" if key == "prores4444" else "prores_422hq"
            trip["decoded"] = compare_planes(planes, truth, source)
        # Reopen: import the export into a fresh project and render it again.
        name = f"reopen_{tier}_{key}"
        media = exported.parent if key == "exr" else exported
        rebuilt = bloom.build([{"project": str(work / "chart-reopen" / name / f"{name}.bloom"),
                                "variant": "A", "media": str(media), "label": name,
                                "width": width, "height": height,
                                "exports": [["exr", "exr", "", "exr/{name}.exr"]]}],
                              work / "chart-reopen")
        reopened = rebuilt.get(name)
        if reopened and reopened.get("ok"):
            planes, error = bloom.render_linear(Path(reopened["project"]),
                                                work / "chart-renders" / name)
            source = ("exr_float_zip" if key == "exr" else
                      "prores_4444" if key == "prores4444" else "prores_422hq")
            trip["reopened"] = ({"error": error} if planes is None else
                                compare_planes(planes, truth, source))
    shutil.rmtree(project.parent / "exports", ignore_errors=True)
    return trips
