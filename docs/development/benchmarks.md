# Performance benchmarks

Status: working

Bloom's standard performance benchmark measures RAM-preview fill, playback and export on a fixed,
regenerable set of footage, and checks colour and format accuracy with a procedural chart. The
tools live in [`tools/bench`](../../tools/bench). The media is never committed: the repository
holds the generators, the manifest that pins and checksums the media, the project builder and the
runner.

| Tool | Runs in | Purpose |
|---|---|---|
| `blender_scene.py` | Blender 4.5 LTS (`blender -b`) | The benchmark scene, rendered with Cycles |
| `chart.py` | Python 3 + numpy | The colour and format accuracy chart and `chart_truth.json` |
| `bench_media.py` | Python 3 | Render, encode, catalogue (`media_manifest.json`) and verify the media |
| `build_projects.py` | Bloom (`bloom-cli run`) | The benchmark projects, built through the operation registry |
| `run_bench.py` | Python 3 | The runner: measurements, results, baseline comparison, chart checks |
| `chart_check.py` | Python 3 + numpy | Chart import, sampling and export round trips (used by the runner) |

## Media

The media root is `--root`/`--media-root`, else `$BLOOM_BENCH_MEDIA`, else `./bench-media`
(ignored by Git). Everything below is relative to it.

### Footage

One deterministic Cycles scene (fixed seed 1729, OptiX GPU rendering, OpenImageDenoise, 128
samples with adaptive threshold 0.02, Standard view transform, transparent film). The camera dollies
across a ground of fine bricks and checks, a frequency-sweep wall, pillars and 360 small saturated
shapes (some emissive, some spinning), past alpha cut-out cards (some moving) and a 45% transparent
veil. A screen-space film grain of about ±7% keeps files from compressing trivially.

| Tier | Size (16:9, square pixels) | Frames at 24 fps | Image sequences | Video |
|---|---|---|---|---|
| `720p` | 1280×720 | 240 | all five | all seven |
| `1080p` | 1920×1080 | 240 | all five | all seven |
| `2160p` | 3840×2160 | 240 | all five | all seven |
| `4320p` (8K, reduced) | 7680×4320 | 48 | PNG16, EXR half PIZ, multilayer EXR | ProRes 4444, HEVC 10-bit |

The 8K tier exists to stress the streaming/memory invariant (output far larger than memory) and the
per-image GPU ceiling; it is generated last and run as its own suite.

One render per frame writes every image format:

- `png8`, `png16`: PNG RGBA 8-bit and 16-bit, sRGB transfer, straight alpha;
- `exr_half_piz`, `exr_float_zip`: OpenEXR RGBA half PIZ and float ZIP, linear Rec.709, premultiplied;
- `exr_multilayer`: the Cycles multilayer EXR (half ZIP; depth, normal, Cryptomatte and the value AOV
  in float): `Composite.Combined`, `ViewLayer.Combined`, `Depth`, `Normal`, `CryptoObject00`/`01`
  with its manifest, AOVs `bench_color` and `bench_id`, and `Noisy Image`.

Video is encoded locally with FFmpeg from the 16-bit PNG master. Codecs without alpha are flattened
over black. All are tagged BT.709 primaries, transfer and matrix, limited range, left chroma siting
(the usual delivery tagging; colour accuracy is the chart's job):

| Format | File | Encoder |
|---|---|---|
| `h264` | `.mp4` | libx264 High, 8-bit 4:2:0, CRF 18 |
| `hevc10` | `.mp4` | libx265 Main 10, 4:2:0, CRF 20 |
| `prores422hq` | `.mov` | prores_ks HQ, 10-bit 4:2:2 |
| `prores4444` | `.mov` | prores_ks 4444 with 16-bit alpha |
| `dnxhr_hqx` | `.mov` | DNxHR HQX, 10-bit 4:2:2 |
| `av1` | `.mp4` | SVT-AV1 preset 8, CRF 30, 10-bit 4:2:0 |
| `vp9` | `.webm` | libvpx-vp9 CRF 31, 8-bit 4:2:0 |

Sizes of the stored set: 720p 8.2 GB, 1080p 17.7 GB, 2160p see `media_manifest.json`
(`totals.bytes` covers everything recorded). The budget is about 200 GB for the footage plus
60-100 GB for the 8K tier.

### Colour and format chart

`chart.py` draws an original procedural chart; every pixel comes from a formula, so every expected
value is exact. Tiers 1080p, 2160p and 4320p, 24 frames each, with a square moving one step per frame
so codecs see motion. Regions, each labelled on the chart:

- 75% colour bars with 40% grey pillars, then 100% primaries, secondaries, white and black;
- grey steps 0-100% and range codes 0, 1, 2, 4, 8, 16 / 235, 239, 247, 251, 254, 255;
- ramps: signal-linear, light-linear, red, green, blue;
- a 24-patch reference set from the published sRGB 8-bit values of the classic 24-patch chart;
- pixel structures in device pixels: 1 px lines, 1 and 2 px checkerboards, a zone plate reaching
  Nyquist, and 1×1/2×2 saturated squares on even and odd columns and rows (chroma siting);
- EXR-only values above 1.0 and below 0;
- alpha ramps and steps (straight in PNG, premultiplied in EXR, an alpha plane in ProRes 4444).

Outputs: PNG16, float EXR (ZIP), and video with explicit tags: H.264 BT.709 limited, BT.709 full
and BT.601 limited; HEVC 10-bit BT.709 limited, BT.2020 PQ and BT.2020 HLG; ProRes 422 HQ;
ProRes 4444; DNxHR HQX (no H.264 at 8K). SDR video carries the same sRGB-transfer signal as the PNG
and is tagged `iec61966-2-1`; HDR video maps linear 1.0 to 203 cd/m² (BT.2408). Chroma is
subsampled by `chart.py` itself (co-sited horizontally, 2-tap vertical average for 4:2:0), so the
Y'CbCr codes are exact before compression.

[`tools/bench/chart_truth.json`](../../tools/bench/chart_truth.json) records, per tier and patch,
the rectangle and an inset sample rectangle, the sRGB signal, linear Rec.709, PNG16 codes, the
premultiplied EXR value and the `[Y, Cb, Cr]` codes of every video variant, plus the formulas of the
ramps and pixel patterns and suggested tolerances. Regenerate it with `chart.py --write-truth`.

### Generating and verifying

```sh
export BLOOM_BENCH_MEDIA=/path/to/bench-media
python3 tools/bench/bench_media.py render --tier 720p --blender /path/to/blender   # Cycles
python3 tools/bench/bench_media.py encode --tier 720p                              # FFmpeg
python3 tools/bench/bench_media.py chart                                           # all chart tiers
python3 tools/bench/bench_media.py manifest --tier 720p \
    --provenance "Blender 4.5.x LTS, Cycles OptiX, <GPU>"
python3 tools/bench/bench_media.py verify
```

`render` needs Blender 4.5 LTS and a Cycles GPU device (`--device OPTIX` by default; `CUDA`,
`HIP`, `METAL`, `ONEAPI` or `CPU` also work but change the result bits). A 240-frame 720p tier takes
about six minutes on an RTX 5080. `encode` needs FFmpeg with libx264, libx265, prores_ks, dnxhd,
libsvtav1 and libvpx-vp9.

The manifest pins the Blender series and the render settings, records the render machine without
host or user names, the FFmpeg version and every encode's arguments, and for each asset its frame
count and numbering, dimensions, format details (PNG depth and channels, EXR compression and channel
list, video codec, profile, pixel format, tags and frame count), size and SHA-256. An asset's
SHA-256 is that of its `sha256sum` listing (one line per file, sorted); the per-file listings are
kept in `<root>/.checksums/`. `remoteObjectStorageUrl` is reserved for a future object-storage mirror
and is unused.

`verify` reports, per asset:

- `verified-exact`: the SHA-256 matches; this is the stored media;
- `regenerated-compatible`: the checksum differs but the frame count, numbering, dimensions and
  format all match. Cycles output differs in its last bits across GPUs, drivers and Blender builds,
  so media regenerated on another machine is acceptable for benchmarking; results record it;
- `incompatible` or `missing`: not usable; the runner skips it.

## Benchmark projects

`build_projects.py` builds two projects per media item through the same operations as the UI,
Python and MCP (it runs inside `bloom-cli`):

- **A, footage only (decode-bound):** a composition made from the footage, one layer.
- **B, a typical shot (compute-bound):** a background solid; the footage enlarged with a Gaussian
  blur; two transformed copies of the footage (scaled, rotated, positioned, one at 92% opacity), the
  smaller one through an alpha track matte from a rotated solid; and an animated text layer.

- **L<n>, many layers (per-layer cost):** `L10`, `L25` and `L50` stack n layers over the 1080p
  EXR half footage: a background solid, the blurred footage, then layers cycling through that
  footage, 1080p PNG16, ProRes 422 HQ and H.264, and 2160p EXR half and ProRes 422 HQ (each
  scaled, rotated and positioned on a grid, with its own opacity and one of six blend modes;
  every fifth layer is a small solid), and an animated text layer on top. They carry only the
  OpenEXR export job.

Each A and B project carries three export jobs (ProRes 422 HQ MOV, H.264 MP4, OpenEXR sequence) written to
`exports/<project>/` next to it, replacing earlier runs and evaluating every frame
(`useCachedFrames` off). The runner builds the projects into its work directory on first use, per
media root, so a run against another root never reuses projects that point elsewhere.

## Running

Build `bloom-cli` (with Python) and the packed RAM-fill harness `bloom_ui_gpu_packed_viewer_test`
in the build directory, then:

```sh
python3 tools/bench/run_bench.py run --suite standard --build-dir build/pw-desktop \
    --media-root "$BLOOM_BENCH_MEDIA" --work-dir bench-work [--gpu-lock-file <lock>]
```

The runner never uses a display: Bloom processes run with `QT_QPA_PLATFORM=offscreen` and without
`DISPLAY`/`WAYLAND_DISPLAY`. Keep the machine otherwise quiet; on a shared machine pass
`--gpu-lock-file` so every Bloom process holds the GPU lock.

| Suite | Tiers | Media | Measurements |
|---|---|---|---|
| `quick` | 1080p | PNG16, EXR half, ProRes 422 HQ, H.264 | A and B; Full and Quarter, cold and warm; playback; ProRes export of the B EXR project |
| `standard` (default) | 720p, 1080p, 2160p | all | A and B; Full, Half, Quarter; cold and warm; playback; all exports of the B projects of PNG16, EXR half, ProRes 422 HQ and H.264 |
| `full` | all four | all | everything, exports for every project |
| `vfx8k` | 4320p | all | everything |
| `layers` | 1080p | EXR half (plus the L mix) | L10, L25, L50; Full, Half, Quarter; cold and warm; cached playback after every fill; playback; no exports |

`--tier`, `--format`, `--variant`, `--resolution`, `--cache`, `--export`, `--no-export` and
`--no-playback` narrow a suite. `--workspace` runs the harness with the application's window
instead of a bare Viewer: `MainWindow` with every foundation editor (Timeline, Properties, Node,
Assets, ...), plus the application's `AssetController`, `AudioPlaybackSession`, live-session
adapter and playback read-ahead, so per-tick work outside the Viewer is measured too. The harness
flag `--bench-workspace-no-assets` leaves the `AssetController` out, to separate its cost.

### What is measured

Every measurement is a fresh process.

- **RAM-preview fill** (`kind: fill`): the harness's `--bench-project` mode opens the project
  through `ProjectHost` and wires the preview like the desktop application (application scheduler
  and budget view, process memory ledger and `MemoryBudgetController`, the qualified display
  bootstrap, settings-derived cache budgets, the packed GPU preview service, fill width from
  measured capacity). After the Viewer's first frame it starts the RAM preview and reports
  `fillFps` = prepared frames / fill seconds at the selected resolution (`Full`, `Half`,
  `Quarter`). **Cold**: the footage is dropped from the OS page cache first
  (`posix_fadvise(DONTNEED)`; `coldDropped` records whether the platform could). **Warm**: the
  footage is in the page cache. Bloom's own caches are always empty because the process is new.
- **Cached playback** (`cachedPlaybackFps`): when a fill completed, the transport plays from the
  start for `--play-seconds` and counts the distinct frames the Viewer actually showed.
  `cachedPresentedFps` counts every change of the shown frame, so it stays meaningful when a
  short work area loops. `cachedPlayUiBusySeconds`, `cachedPlayMaxSliceMs` and
  `cachedPlayLongSlices` report the UI thread's event-loop time over the run, its longest slice
  and the slices longer than one frame period. `cachedPlayCacheHits`/`Misses`/`StaleDrops`/
  `Evictions`, `cachedPlayRevisionChanges` and `cachedPlaySnapshotRefreshes` say whether a cached
  run really stayed cached, and `lastFrameDiagnostics` carries the last frame's preview
  diagnostics. `realTimeRatio` (playback runs) and `cachedRealTimeRatio` are the session time the
  transport advanced, unwrapped across loop restarts, over wall time: 1.0 means playback held real
  time by dropping frames rather than slowing down.
- **Playback** (`kind: playback`): a cold start with background read-ahead and no RAM fill;
  `playbackFps` = distinct frames shown / seconds, next to the frames the transport asked for.
- **Export** (`kind: export`): `bloom-cli export --job <id> --replace`; `secondsPerFrame` = process
  wall time / frames, including start-up. The export's own provenance gives `gpuFrames` and the
  CPU-reference frame count (`cpuFallbackFrames`). An encoder that is not installed is recorded as
  `outcome: unavailable` (H.264 needs Cisco OpenH264, which Bloom installs only with the user's
  consent; HEVC needs a VA-API encoder).
- **GPU stages**: `uploadSubmitSeconds`, `uploadFenceSeconds`, `processingSubmitSeconds`,
  `processingFenceSeconds` and `packedTransferSeconds` are the device owner's wall time per stage
  over the measured window (from the preview service counters), with `residentGraphJobs` and
  `nativeDispatches`.
- **Resources**: `gpuBusyPercent` (and peak) from `nvidia-smi` sampled every 200 ms over the
  measured window (NVIDIA only; absent elsewhere); `fillCoresBusy` = process CPU seconds / wall
  seconds of the fill; `cpuCoresBusy` for exports from process-tree sampling; `peakRssBytes` is the
  harness process peak (`getrusage`, or the peak working set on Windows) and, for exports, the
  sampled peak of the process tree; `cpuFallbackFrames` must be 0; `admissionRefusals` and
  `residentDetail` report GPU admission refusals and the resident path's last detail (the 8K tier's
  per-image ceiling shows up here).

Process sampling uses `/proc` on Linux and `psutil` where it is installed; without either, CPU and
RSS sampling are reported as absent rather than guessed.

### Results

Each run writes `<work-dir>/results/<UTC timestamp>[-label].json`, a `.md` summary table and a
`.log` with every command and its output. The JSON holds:

- `machine`: CPU, logical cores, RAM, GPU name, driver and VRAM, OS;
- `bloom`: the source commit of the build and whether its tree was dirty;
- `media`: the root and its verification status per asset (`verified-exact` or
  `regenerated-compatible`);
- `measurements`: one row per measurement, keyed by tier, format, variant (A/B), kind, resolution,
  cache and export, with the fields above;
- `comparison` when `--baseline` was given.

### Baselines and storage comparisons

```sh
python3 tools/bench/run_bench.py run --baseline bench-work/results/<old>.json --threshold 10
python3 tools/bench/run_bench.py compare <new>.json <old>.json --threshold 10
```

A regression is a measurement whose fill fps, playback fps or cached playback fps fell, or whose
export seconds/frame or peak RSS rose, by more than the threshold (percent). Any CPU-fallback frame
is reported separately. Either makes the command exit non-zero.

To compare storage, run the same suite with `--media-root` pointing at the other location (for
example a network share holding a copy of the media) and a `--label`; the media is verified there
first, and its projects are built separately. Compare the two results files with `compare`.

### Chart checks

```sh
python3 tools/bench/run_bench.py chart --media-root "$BLOOM_BENCH_MEDIA" --tier 1080p
```

For each chart asset it builds a project, renders frame 1 to a linear Rec.709 float EXR and reports
per patch the error in 8-bit sRGB code units (relative error for the EXR-only HDR values), alpha
error, and the siting shift: the centroid of each tiny square against its true centre, in pixels
(`maxAbsDx`, `maxAbsDy`). Tolerances: 0.5 codes for PNG16 and EXR, 3 for 8-bit video, 2 for 10-bit,
0.35 px for siting. From the PNG16 chart it then exports ProRes 422 HQ, ProRes 4444, DNxHR HQX,
H.264 and OpenEXR; each export's tags are probed and its first frame decoded with FFmpeg by those
tags (sRGB transfer, or BT.1886 for BT.709-tagged files) and compared, and the export is imported
back into Bloom and compared again. Results go to `<work-dir>/results/<timestamp>-chart.json` and
`.md`; the command exits non-zero when any import fails a patch or siting check.

## Platform notes

The tools are Python and run on Linux, macOS and Windows. Cold runs need `posix_fadvise` (Linux);
elsewhere they are recorded with `coldDropped: false`. GPU busy needs `nvidia-smi`. Process
sampling needs `/proc` or `psutil`. The remote-render orchestration used to produce the stored set
is not part of the repository; `bench_media.py render` is the supported way to regenerate locally.
