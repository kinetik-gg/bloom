#!/usr/bin/env python3
"""Bloom performance benchmark runner.

For each selected media item it builds two projects headlessly (A: footage only, decode-bound;
B: a typical shot, compute-bound; see build_projects.py) and measures, each in a fresh process:

- RAM-preview fill fps at Full/Half/Quarter, cold (footage dropped from the OS page cache) and
  warm (footage in the page cache; caches inside Bloom are always empty: new process);
- playback fps: transport from a cold start with background read-ahead, and from the full RAM
  cache right after each fill;
- export seconds/frame to ProRes 422 HQ, H.264 MP4 and OpenEXR (bloom-cli export);
- GPU busy % (nvidia-smi sampling over the measured window), CPU cores busy, peak RSS and the
  CPU-fallback frame count, which must be 0.

The RAM-fill and playback measurements use the --bench-project mode of the packed RAM-fill
harness (bloom_ui_gpu_packed_viewer_test), which is wired like the desktop application.

    run_bench.py run [--suite quick|standard|full] [--media-root R] [--build-dir B]
                     [--baseline old.json --threshold 10] [--label nas]
    run_bench.py compare new.json old.json [--threshold 10]
    run_bench.py chart  ...   (colour/format accuracy checks; see chart_check.py)

Results: <work-dir>/results/<UTC timestamp>[-label].json plus a .md summary table.
"""
import argparse
import datetime
import fcntl
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_media  # noqa: E402
import bench_spec as spec  # noqa: E402

REPO = HERE.parent.parent
OFFSCREEN_ENV = {"QT_QPA_PLATFORM": "offscreen", "QT_QPA_PLATFORMTHEME": "generic"}
RESOLUTIONS = ("Full", "Half", "Quarter")
EXPORTS = ("prores422hq", "h264", "exr")
SUITES = {
    # suite: (tiers, formats or None for all, variants, resolutions, caches, playback, exports,
    #         export variants, export formats or None for all)
    "quick": (("1080p",), ("png16", "exr_half_piz", "prores422hq", "h264"), ("A", "B"),
              ("Full", "Quarter"), ("cold", "warm"), True, ("prores422hq",), ("B",),
              ("exr_half_piz",)),
    "standard": (("720p", "1080p", "2160p"), None, ("A", "B"), RESOLUTIONS, ("cold", "warm"),
                 True, EXPORTS, ("B",), ("png16", "exr_half_piz", "prores422hq", "h264")),
    "full": (spec.TIER_ORDER, None, ("A", "B"), RESOLUTIONS, ("cold", "warm"), True, EXPORTS,
             ("A", "B"), None),
    # The reduced 8K tier on its own: streaming/memory invariant and per-image GPU ceiling.
    "vfx8k": (("4320p",), None, ("A", "B"), RESOLUTIONS, ("cold", "warm"), True, EXPORTS,
              ("A", "B"), None),
}
# metric: True when higher is better
COMPARED = {"fillFps": True, "playbackFps": True, "cachedPlaybackFps": True,
            "secondsPerFrame": False, "peakRssBytes": False}

# --- environment --------------------------------------------------------------------------------


def run_text(command, **kwargs):
    try:
        return subprocess.run(command, capture_output=True, text=True, check=True,
                              **kwargs).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def machine_info():
    info = {"os": platform.platform(), "python": platform.python_version()}
    try:
        cpuinfo = Path("/proc/cpuinfo").read_text()
        model = re.search(r"model name\s*:\s*(.+)", cpuinfo)
        info["cpu"] = model.group(1).strip() if model else platform.processor()
    except OSError:
        info["cpu"] = platform.processor()
    info["logicalCores"] = os.cpu_count()
    try:
        total = re.search(r"MemTotal:\s*(\d+) kB", Path("/proc/meminfo").read_text())
        info["ramBytes"] = int(total.group(1)) * 1024
    except (OSError, AttributeError):
        pass
    gpu = run_text(["nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                    "--format=csv,noheader,nounits"])
    if gpu:
        name, driver, memory = [part.strip() for part in gpu.splitlines()[0].split(",")]
        info["gpu"] = {"name": name, "driver": driver, "vramBytes": int(memory) * 1 << 20}
    return info


def bloom_revision(build_dir):
    source = REPO
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        found = re.search(r"CMAKE_HOME_DIRECTORY:INTERNAL=(.+)", cache.read_text())
        if found:
            source = Path(found.group(1))
    commit = run_text(["git", "-C", str(source), "rev-parse", "HEAD"])
    dirty = run_text(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"])
    return {"commit": commit, "dirty": bool(dirty)}


def find_loader(build_dir, explicit):
    if explicit:
        return Path(explicit)
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        found = re.search(r"BLOOM_DEPENDENCY_PREFIX:\w+=(.+)", cache.read_text())
        if found:
            for name in ("lib/libvulkan.so.1", "lib/libvulkan.1.dylib", "bin/vulkan-1.dll"):
                if (Path(found.group(1)) / name).is_file():
                    return Path(found.group(1)) / name
    raise SystemExit("cannot find the bundled Vulkan loader; pass --loader")


class GpuLock:
    """Optional machine GPU lock, held around every Bloom process."""

    def __init__(self, path):
        self.path = path
        self.handle = None

    def __enter__(self):
        if self.path:
            self.handle = open(self.path, "a+")
            fcntl.flock(self.handle, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        if self.handle:
            fcntl.flock(self.handle, fcntl.LOCK_UN)
            self.handle.close()

# --- sampling -----------------------------------------------------------------------------------


def process_tree(root_pid):
    children = {}
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
                children.setdefault(int(fields[1]), []).append(int(entry.name))
            except (OSError, IndexError):
                continue
    tree, stack = [], [root_pid]
    while stack:
        pid = stack.pop()
        tree.append(pid)
        stack.extend(children.get(pid, []))
    return tree


try:
    import psutil  # optional: process sampling on macOS and Windows
except ImportError:
    psutil = None


def tree_usage(root_pid):
    """(cpu seconds, rss bytes) summed over a process tree; (None, None) when unmeasurable."""
    if psutil is not None:
        try:
            root = psutil.Process(root_pid)
            processes = [root] + root.children(recursive=True)
        except psutil.Error:
            return None, None
        cpu = rss = 0
        for process in processes:
            try:
                times = process.cpu_times()
                cpu += times.user + times.system
                rss += process.memory_info().rss
            except psutil.Error:
                continue
        return cpu, rss
    if not Path("/proc/self/stat").exists():
        return None, None
    ticks = os.sysconf("SC_CLK_TCK")
    page = os.sysconf("SC_PAGE_SIZE")
    cpu = rss = 0
    for pid in process_tree(root_pid):
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            cpu += (int(fields[11]) + int(fields[12])) / ticks
            rss += int(fields[21]) * page
        except (OSError, IndexError, ValueError):
            continue
    return cpu, rss


class Sampler:
    """Samples GPU utilisation (nvidia-smi) and the process tree's CPU/RSS every 200 ms."""

    def __init__(self, pid):
        self.pid = pid
        self.samples = []  # (epoch, gpu %, cpu seconds, rss bytes)
        self.stop = threading.Event()
        self.gpu = None
        self.gpu_value = None
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def start(self):
        if shutil.which("nvidia-smi"):
            self.gpu = subprocess.Popen(
                ["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits",
                 "-lms", "200"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            threading.Thread(target=self.read_gpu, daemon=True).start()
        self.thread.start()
        return self

    def read_gpu(self):
        for line in self.gpu.stdout:
            try:
                self.gpu_value = float(line.split(",")[0])
            except ValueError:
                pass

    def loop(self):
        while not self.stop.is_set():
            cpu, rss = tree_usage(self.pid)
            self.samples.append((time.time(), self.gpu_value, cpu, rss))
            self.stop.wait(0.2)

    def finish(self):
        self.stop.set()
        self.thread.join()
        if self.gpu:
            self.gpu.terminate()
            self.gpu.wait()

    def window(self, start=None, end=None):
        rows = [s for s in self.samples if (start is None or s[0] >= start) and
                (end is None or s[0] <= end)]
        result = {"peakTreeRssBytes": max((s[3] for s in self.samples if s[3] is not None),
                                          default=None)}
        gpu = [s[1] for s in rows if s[1] is not None]
        if gpu:
            result["gpuBusyPercent"] = round(sum(gpu) / len(gpu), 1)
            result["gpuBusyPeakPercent"] = max(gpu)
        rows = [s for s in rows if s[2] is not None]
        if len(rows) >= 2 and rows[-1][0] > rows[0][0]:
            result["cpuCoresBusy"] = round((rows[-1][2] - rows[0][2]) / (rows[-1][0] - rows[0][0]),
                                           2)
        return result

# --- page cache ---------------------------------------------------------------------------------


def drop_page_cache(paths):
    """Cold start: drop the footage from the OS page cache (posix_fadvise DONTNEED).

    Returns False where the platform has no such call (macOS, Windows): the run is then
    recorded with coldDropped=false and is not a true cold read.
    """
    if not hasattr(os, "posix_fadvise"):
        return False
    for path in paths:
        try:
            descriptor = os.open(path, os.O_RDONLY)
        except OSError:
            continue
        try:
            os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(descriptor)
    return True


def media_files(root, entry):
    if entry["type"] == "video":
        return [root / entry["path"]]
    return [path for _, path in bench_media.sequence_files(root, entry["path"])]

# --- measurements -------------------------------------------------------------------------------


def run_measured(command, env, timeout, lock):
    with lock:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, env=env)
        sampler = Sampler(process.pid).start()
        started = time.time()
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()
            output += "\nBENCH_ERROR timeout"
        sampler.finish()
        return process.returncode, output, sampler, time.time() - started


def bench_env():
    env = {key: value for key, value in os.environ.items()
           if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(OFFSCREEN_ENV)
    return env


def harness_measure(args, project, resolution, playback, log):
    command = [str(args.harness), "--loader", str(args.loader), "--require-device",
               "--bench-project", str(project), "--bench-timeout-seconds", str(args.timeout)]
    if resolution == "Half":
        command.append("--cache100-half")
    elif resolution == "Quarter":
        command.append("--quarter")
    # Playback is timed in playback runs and after Full fills; other fills skip it.
    seconds = args.play_seconds if playback or resolution == "Full" else 0
    command += ["--bench-play-seconds", str(seconds)]
    if playback:
        command.append("--bench-playback")
    code, output, sampler, wall = run_measured(command, bench_env(), args.timeout + 300,
                                               args.lock)
    log.write(f"$ {' '.join(command)}\n{output}\n")
    found = re.search(r"^BENCH_RESULT (\{.*\})$", output, re.MULTILINE)
    result = {"exitCode": code, "processWallSeconds": round(wall, 2)}
    if found:
        result.update(json.loads(found.group(1)))
        result.update(sampler.window(result.get("windowStartEpoch"), result.get("windowEndEpoch")))
    else:
        result["error"] = (re.findall(r"^(?:BENCH_ERROR|Unsupported).*$", output, re.MULTILINE)
                           or output.strip().splitlines()[-3:])
        result.update(sampler.window())
    return result


def export_measure(args, project, job, frames, log):
    command = [str(args.bloom_cli), "export", "--project", str(project), "--job", str(job),
               "--replace"]
    code, output, sampler, wall = run_measured(command, bench_env(), args.timeout, args.lock)
    log.write(f"$ {' '.join(command)}\n{output}\n")
    result = {"exitCode": code, "seconds": round(wall, 3), "frames": frames,
              "secondsPerFrame": round(wall / frames, 4) if frames else None}
    result.update(sampler.window())
    result["peakRssBytes"] = result.pop("peakTreeRssBytes")
    result["peakRssScope"] = "process tree (sampled every 200 ms)"
    events = re.search(r'^(\{"lastSequence".*\})$', output, re.MULTILINE)
    if events:
        for event in json.loads(events.group(1)).get("events", []):
            if event.get("type") in ("succeeded", "failed", "cancelled"):
                result["outcome"] = event["type"]
                if event.get("reason"):
                    result["reason"] = event["reason"]
                for out in event.get("outputs", []):
                    provenance = out.get("provenance", {})
                    result["gpuFrames"] = provenance.get("gpuFrames")
                    result["cpuFallbackFrames"] = provenance.get("cpuReferenceFrames")
                    result["artifactBytes"] = out.get("artifacts", {}).get("bytes")
    unavailable = re.search(r"^error\[bloom-cli\.export[^\]]*\]: (.*)$", output, re.MULTILINE)
    if unavailable:
        result["outcome"] = "unavailable"
        result["reason"] = unavailable.group(1)
    if code != 0:
        result["error"] = result.get("reason") or output.strip().splitlines()[-3:]
        result["secondsPerFrame"] = None
    return result

# --- suite --------------------------------------------------------------------------------------


def select_assets(manifest, args):
    tiers, formats, *_ = SUITES[args.suite]
    tiers = args.tier or tiers
    formats = args.format or formats
    for asset_id, entry in manifest["assets"].items():
        if entry["kind"] == "footage" and entry["tier"] in tiers and (
                formats is None or entry["format"] in formats):
            yield asset_id, entry


def build_projects(args, root, selected, projects_dir, log):
    jobs, plan = [], []
    for asset_id, entry in selected:
        for variant in args.variants:
            name = f"{entry['tier']}_{entry['format']}_{variant}"
            project = projects_dir / name / f"{name}.bloom"
            media = root / (entry["path"] if entry["type"] == "video"
                            else str(Path(entry["path"]).parent))
            plan.append({"assetId": asset_id, "entry": entry, "variant": variant,
                         "project": project})
            if args.rebuild_projects or not project.is_file():
                jobs.append({"project": str(project), "variant": variant,
                             "media": str(media.resolve()),
                             "label": f"{entry['tier']} {entry['format']}".upper(),
                             "width": entry["width"], "height": entry["height"]})
    report = projects_dir / "build-report.json"
    failures = {}
    if jobs:
        projects_dir.mkdir(parents=True, exist_ok=True)
        spec_file = projects_dir / "build-jobs.json"
        spec_file.write_text(json.dumps(jobs, indent=1))
        env = bench_env()
        env["BLOOM_BENCH_BUILD"] = str(spec_file)
        env["BLOOM_BENCH_BUILD_REPORT"] = str(report)
        print(f"building {len(jobs)} project(s)", flush=True)
        output = subprocess.run([str(args.bloom_cli), "run", str(HERE / "build_projects.py")],
                                env=env, capture_output=True, text=True)
        log.write(output.stdout + output.stderr)
        built = json.loads(report.read_text()) if report.is_file() else []
        if not built:
            raise SystemExit(f"project build failed: {output.stderr[-2000:]}")
        for row in built:
            if row.get("ok"):
                stamp = Path(row["project"]).with_suffix(".jobs.json")
                stamp.write_text(json.dumps(row["exportJobs"]))
            else:
                # A medium Bloom cannot import is a result, not a reason to stop the run.
                failures[row["project"]] = row.get("error")
    for item in plan:
        stamp = item["project"].with_suffix(".jobs.json")
        item["exportJobs"] = json.loads(stamp.read_text()) if stamp.is_file() else {}
        if not stamp.is_file():
            item["buildError"] = failures.get(str(item["project"]), "the project was not built")
    return plan


def summarise(measurements):
    rows = []
    for m in measurements:
        key = f"{m['tier']} {m['format']} {m['variant']}"
        if m["kind"] == "fill":
            value = m.get("fillFps")
            detail = (f"{value:.1f} fps" if value is not None else "error") + (
                f", {m.get('preparedFrames')}/{m.get('totalFrames')} prepared"
                f", cached play {m.get('cachedPlaybackFps', 0):.1f} fps"
                if value is not None else "")
            rows.append((key, f"fill {m['resolution']} {m['cache']}", detail, m))
        elif m["kind"] == "playback":
            value = m.get("playbackFps")
            rows.append((key, f"playback {m['resolution']} {m['cache']}",
                         f"{value:.1f} fps" if value is not None else "error", m))
        elif m["kind"] == "build":
            rows.append((key, "project build", f"error: {m.get('error')}", m))
        else:
            value = m.get("secondsPerFrame")
            rows.append((key, f"export {m['export']}",
                         f"{value * 1000:.0f} ms/frame" if value and not m.get("error")
                         else "error", m))
    lines = ["| media | measurement | result | GPU % | cores | peak RSS GB | CPU-fallback |",
             "|---|---|---|---|---|---|---|"]
    for key, what, detail, m in rows:
        rss = m.get("peakRssBytes") or m.get("peakTreeRssBytes") or 0
        cores = m.get("fillCoresBusy", m.get("cpuCoresBusy"))
        cores = f"{cores:.1f}" if isinstance(cores, (int, float)) else ""
        lines.append(f"| {key} | {what} | {detail} | {m.get('gpuBusyPercent', '')} | {cores} | "
                     f"{rss / 1e9:.2f} | {m.get('cpuFallbackFrames', '')} |")
    return "\n".join(lines)


def measurement_key(m):
    return (m["tier"], m["format"], m["variant"], m["kind"], m.get("resolution"),
            m.get("cache"), m.get("export"))


def compare(current, baseline, threshold):
    old = {measurement_key(m): m for m in baseline["measurements"]}
    regressions, improvements = [], []
    for m in current["measurements"]:
        before = old.get(measurement_key(m))
        if not before:
            continue
        for metric, higher_better in COMPARED.items():
            new_value, old_value = m.get(metric), before.get(metric)
            if not new_value or not old_value:
                continue
            change = (new_value - old_value) / old_value * 100
            worse = -change if higher_better else change
            row = {"key": " ".join(str(k) for k in measurement_key(m) if k), "metric": metric,
                   "baseline": old_value, "current": new_value, "changePercent": round(change, 1)}
            if worse > threshold:
                regressions.append(row)
            elif -worse > threshold:
                improvements.append(row)
    fallbacks = [" ".join(str(k) for k in measurement_key(m) if k) for m in current["measurements"]
                 if m.get("cpuFallbackFrames")]
    return {"threshold": threshold, "regressions": regressions, "improvements": improvements,
            "cpuFallbackMeasurements": fallbacks}


def command_run(args):
    (tiers, formats, variants, resolutions, caches, playback, exports, export_variants,
     export_formats) = SUITES[args.suite]
    args.variants = args.variant or variants
    resolutions = args.resolution or resolutions
    caches = args.cache or caches
    exports = () if args.no_export else (args.export or exports)
    root = bench_media.media_root(args.media_root).resolve()
    build_dir = prepare_binaries(args)
    for binary in (args.bloom_cli, args.harness):
        if not binary.is_file():
            raise SystemExit(f"missing {binary}; build bloom-cli and "
                             "bloom_ui_gpu_packed_viewer_test first")
    manifest = json.loads(Path(args.manifest).read_text())
    selected = list(select_assets(manifest, args))
    if not selected:
        raise SystemExit("no media selected")
    work = Path(args.work_dir).resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    name = stamp + (f"-{args.label}" if args.label else "")
    results_dir = work / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    log = open(results_dir / f"{name}.log", "w")
    print(f"verifying {len(selected)} media item(s) under {root}", flush=True)
    selected_ids = {asset_id for asset_id, _ in selected}
    verification = bench_media.verify(root, manifest, lambda i, _: i in selected_ids,
                                      progress=False)
    usable = {i for i, r in verification["assets"].items()
              if r["status"] in ("verified-exact", "regenerated-compatible")}
    print(f"media: {verification['status']}", flush=True)
    selected = [(i, e) for i, e in selected if i in usable]
    projects_dir = work / "projects" / re.sub(r"[^A-Za-z0-9]+", "_", root.name or "root")
    plan = build_projects(args, root, selected, projects_dir, log)
    if args.build_only:
        for item in plan:
            print(f"project {item['project']} exports={item['exportJobs']}")
        return 0
    results = {"schema": "bloom-bench-results/1", "startedAt": stamp, "label": args.label,
               "note": args.note,
               "suite": args.suite, "machine": machine_info(), "bloom": bloom_revision(build_dir),
               "media": {"root": str(root), "verification": verification["status"],
                         "assets": verification["assets"]},
               "settings": {"resolutions": list(resolutions), "caches": list(caches),
                            "playSeconds": args.play_seconds, "exports": list(exports)},
               "measurements": []}
    out_json = results_dir / f"{name}.json"

    def record(item, kind, values):
        entry = item["entry"]
        row = {"tier": entry["tier"], "format": entry["format"], "variant": item["variant"],
               "kind": kind, **values}
        results["measurements"].append(row)
        out_json.write_text(json.dumps(results, indent=1) + "\n")
        print(f"  {kind:<8} {row.get('resolution', '')} {row.get('cache', '')} "
              f"{row.get('export', '')} -> " + ", ".join(
                  f"{k}={row[k]}" for k in ("fillFps", "playbackFps", "cachedPlaybackFps",
                                            "secondsPerFrame", "cpuFallbackFrames",
                                            "gpuBusyPercent", "error") if k in row), flush=True)

    for item in plan:
        entry = item["entry"]
        files = media_files(root, entry)
        print(f"{item['assetId']} {item['variant']}", flush=True)
        if "buildError" in item:
            record(item, "build", {"error": item["buildError"]})
            continue
        for resolution in resolutions:
            for cache in caches:
                dropped = drop_page_cache(files) if cache == "cold" else None
                values = harness_measure(args, item["project"], resolution, False, log)
                record(item, "fill", {"resolution": resolution, "cache": cache,
                                      "coldDropped": dropped, **values})
        if playback and not args.no_playback:
            dropped = drop_page_cache(files)
            values = harness_measure(args, item["project"], "Full", True, log)
            record(item, "playback", {"resolution": "Full", "cache": "cold",
                                      "coldDropped": dropped, **values})
        if item["variant"] in export_variants and (export_formats is None or
                                                   entry["format"] in export_formats):
            for export in exports:
                job = item["exportJobs"].get(export)
                if job is None:
                    continue
                values = export_measure(args, item["project"], job, entry["frames"], log)
                record(item, "export", {"export": export, **values})
                shutil.rmtree(item["project"].parent / "exports", ignore_errors=True)
    results["finishedAt"] = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ")
    if args.baseline:
        results["comparison"] = compare(results, json.loads(Path(args.baseline).read_text()),
                                        args.threshold)
    out_json.write_text(json.dumps(results, indent=1) + "\n")
    table = summarise(results["measurements"])
    header = (f"# Bloom benchmark {name}\n\nBloom {results['bloom']['commit']}"
              f"{' (dirty)' if results['bloom']['dirty'] else ''}; media "
              f"{results['media']['verification']}; GPU "
              f"{results['machine'].get('gpu', {}).get('name', '?')}; CPU "
              f"{results['machine'].get('cpu')}\n\n")
    (results_dir / f"{name}.md").write_text(header + table + "\n")
    print(table)
    print(f"results: {out_json}")
    if "comparison" in results:
        report_comparison(results["comparison"])
        return 1 if results["comparison"]["regressions"] or results["comparison"][
            "cpuFallbackMeasurements"] else 0
    return 0


def report_comparison(comparison):
    for row in comparison["regressions"]:
        print(f"REGRESSION {row['key']} {row['metric']}: {row['baseline']} -> {row['current']} "
              f"({row['changePercent']:+.1f}%)")
    for key in comparison["cpuFallbackMeasurements"]:
        print(f"CPU-FALLBACK {key}")
    print(f"{len(comparison['regressions'])} regression(s) over {comparison['threshold']}%, "
          f"{len(comparison['improvements'])} improvement(s)")


def executable(path):
    windows = path.with_suffix(".exe")
    return windows if not path.is_file() and windows.is_file() else path


def prepare_binaries(args):
    build_dir = Path(args.build_dir).resolve()
    args.bloom_cli = executable(build_dir / "apps/bloom-cli/bloom-cli")
    args.harness = executable(build_dir / "src/ui/tests/bloom_ui_gpu_packed_viewer_test")
    args.loader = find_loader(build_dir, args.loader)
    args.lock = GpuLock(args.gpu_lock_file)
    return build_dir


def command_chart(args):
    import chart_check
    build_dir = prepare_binaries(args)
    if not args.bloom_cli.is_file():
        raise SystemExit(f"missing {args.bloom_cli}")
    root = bench_media.media_root(args.media_root).resolve()
    work = Path(args.work_dir).resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    name = f"{stamp}-chart" + (f"-{args.label}" if args.label else "")
    tiers = args.tier or ["1080p", "2160p", "4320p"]
    results, table = chart_check.run_chart(args, root, work, tiers)
    document = {"schema": "bloom-chart-results/1", "startedAt": stamp, "label": args.label,
                "machine": machine_info(), "bloom": bloom_revision(build_dir),
                "media": {"root": str(root)}, "tolerances": chart_check.TOLERANCE,
                **results}
    results_dir = work / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    (results_dir / f"{name}.json").write_text(json.dumps(document, indent=1) + "\n")
    (results_dir / f"{name}.md").write_text(f"# Bloom chart check {name}\n\n{table}\n")
    print(table)
    print(f"results: {results_dir / (name + '.json')}")
    failed = [key for key, row in results["imports"].items()
              if "error" in row or row["failedPatches"] or not row["siting"]["pass"]]
    return 1 if failed else 0


def command_compare(args):
    comparison = compare(json.loads(Path(args.current).read_text()),
                         json.loads(Path(args.baseline).read_text()), args.threshold)
    report_comparison(comparison)
    return 1 if comparison["regressions"] or comparison["cpuFallbackMeasurements"] else 0


def command_merge(args):
    """Concatenate the measurements of several runs (for example tiers run separately)."""
    documents = [json.loads(Path(path).read_text()) for path in args.results]
    merged = dict(documents[0])
    merged["measurements"] = [m for document in documents for m in document["measurements"]]
    merged["mergedFrom"] = [{"startedAt": d["startedAt"], "label": d.get("label"),
                             "note": d.get("note"), "bloom": d["bloom"]} for d in documents]
    merged["media"] = dict(documents[0]["media"])
    merged["media"]["assets"] = {k: v for d in documents for k, v in d["media"]["assets"].items()}
    statuses = {d["media"]["verification"] for d in documents}
    merged["media"]["verification"] = ("verified-exact" if statuses == {"verified-exact"} else
                                       "regenerated-compatible" if "failed" not in statuses
                                       else "failed")
    Path(args.output).write_text(json.dumps(merged, indent=1) + "\n")
    Path(args.output).with_suffix(".md").write_text(summarise(merged["measurements"]) + "\n")
    print(f"merged {len(documents)} runs, {len(merged['measurements'])} measurements")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="run a benchmark suite")
    run.add_argument("--suite", choices=sorted(SUITES), default="standard")
    run.add_argument("--media-root", help=f"media root (default ${spec.MEDIA_ROOT_ENV} or "
                                          f"./{spec.DEFAULT_MEDIA_ROOT}); point it at a share "
                                          "to compare storage")
    run.add_argument("--manifest", default=str(bench_media.MANIFEST))
    run.add_argument("--build-dir", default="build/pw-desktop")
    run.add_argument("--work-dir", default="bench-work",
                     help="projects, exports and results go here")
    run.add_argument("--loader", help="Vulkan loader (default: the build's dependency prefix)")
    run.add_argument("--gpu-lock-file", help="flock this file around every Bloom process")
    run.add_argument("--label", help="suffix for the results file, e.g. 'nas'")
    run.add_argument("--note", help="free text stored in the results, e.g. memory limits")
    run.add_argument("--tier", action="append", choices=spec.TIER_ORDER)
    run.add_argument("--format", action="append",
                     choices=spec.IMAGE_FORMATS + spec.VIDEO_FORMATS)
    run.add_argument("--variant", action="append", choices=("A", "B"))
    run.add_argument("--resolution", action="append", choices=RESOLUTIONS)
    run.add_argument("--cache", action="append", choices=("cold", "warm"))
    run.add_argument("--export", action="append", choices=EXPORTS)
    run.add_argument("--no-export", action="store_true")
    run.add_argument("--no-playback", action="store_true")
    run.add_argument("--play-seconds", type=float, default=6.0)
    run.add_argument("--timeout", type=int, default=1800, help="per process, seconds")
    run.add_argument("--rebuild-projects", action="store_true")
    run.add_argument("--build-only", action="store_true",
                     help="verify the media and build the projects, measure nothing")
    run.add_argument("--baseline", help="compare against this results JSON")
    run.add_argument("--threshold", type=float, default=10.0, help="regression threshold, %%")
    chk = sub.add_parser("chart", help="colour/format accuracy checks with the chart")
    chk.add_argument("--media-root")
    chk.add_argument("--build-dir", default="build/pw-desktop")
    chk.add_argument("--work-dir", default="bench-work")
    chk.add_argument("--loader")
    chk.add_argument("--gpu-lock-file")
    chk.add_argument("--label")
    chk.add_argument("--tier", action="append", choices=("1080p", "2160p", "4320p"))
    mrg = sub.add_parser("merge", help="merge results files into one")
    mrg.add_argument("results", nargs="+")
    mrg.add_argument("-o", "--output", required=True)
    cmp = sub.add_parser("compare", help="compare two results files")
    cmp.add_argument("current")
    cmp.add_argument("baseline")
    cmp.add_argument("--threshold", type=float, default=10.0)
    args = parser.parse_args()
    sys.exit({"run": command_run, "chart": command_chart, "merge": command_merge,
              "compare": command_compare}[args.command](args))


if __name__ == "__main__":
    main()
