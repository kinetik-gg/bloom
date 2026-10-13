# Issue 304: Linux implementation and qualification

Status: Linux implementation and local qualification complete

Updated: 2026-10-14

This record owns the current Linux evidence for
[issue #304](https://github.com/kinetik-gg/bloom/issues/304). The owner narrowed this execution to
Linux. Native Windows/macOS execution and their hosted qualification remain explicitly unqualified.
The historical checkpoints in [the overhaul plan](gpu-reliability-and-preview.md) retain their
original scope; this record does not retrospectively enlarge those results.

## Allocation ownership

Processing buffers reserve before native allocation, reconcile the actual VMA suballocation and
keep the exact owning charge through partial construction, cache publication, aliases, cancellation,
foreign-thread release and uncertain completion. The covered families are resident input/output/
status, composite/affine/blend/axis/map intermediates, vector coverage and palettes, and OCIO packed/
uniform/status/LUT staging and scratch. Successful cached storage promotes its actual backing out of
the originating phase; failed partial allocations can return only to that same live phase.

Bloom-owned presentation images, staging and uniform buffers use independent admission against the
same root. External presentation-engine images have explicit unowned provenance and unknown physical
size. Root totals, allocator-requested payload, committed allocator blocks, independent driver heap
usage/budget and process RSS are separate measurements. None is a substitute for another, and these
checks do not establish a whole-process memory ceiling.

The native processing, image, operation, OCIO, presentation, queue-timing and allocator suites pass
with a required hardware device. Their directed controls cover low budgets, failed construction,
same-root/device affinity, cancellation, device loss, unknown fences, owner retirement and reuse.
See [the native ownership contracts](../architecture/gpu-backend.md).

SourceOver also checks the supported signed shader-coordinate domain before pixel allocation or
submission: unrepresentable origin differences and local-coordinate additions are refused, while
small exact differences near the signed 64-bit origin limit remain supported. The serial and batch
paths share this check.

## Viewer and retained playback

The opt-in packed Viewer fixture uses the actual preview service, controller, widget paint path and
final evaluator. One hundred distinct generated 1920 × 1080 EXR members produce Half 960 × 540
frames; an independent still-member CPU evaluation checks the selected pixels. All ten equivalent
fill/purge cycles use the same root/service/controller. Purge waits for scheduler/native retirement
and preserves only the current displayed picture.

Both measured widths return to exactly 2,073,680 host-owned bytes and 2,073,616 device-owned bytes
after each purge, with zero admitted or unproven phases. The retained set contains 207,360,000 packed
pixel bytes. After the cycles, continuous looping playback runs for 60 seconds and observes 1,500
actual widget-painted times (25 per second), with no new native dispatch or transfer and no retained
payload growth. Physical scanout is unmeasured.

The fixture exercises selection, held body dragging, scrub during fill, a concurrent real final
EXR export, a committed one-step undoable drag, widget resize and document replacement during an
active refill. Published image identity, selected geometry and replacement generation must agree.
Scalar probe identity now holds a weak frame reference; the packed path releases its obsolete native
request correlation frame after retiring the native target. These fixes remove a second hidden
packed-picture owner found by repeated purge testing.

The final source-aligned width-one run with native batching enabled passes all ten cycles and the
60-second playback check. Median fill is 5,211.405 ms. Each purge returns the same exact host/device
claims above and 2,073,616 bytes of VMA payload; retained VMA blocks are 201,326,592 bytes. Independent
driver heap usage is 304,349,184 bytes, explicitly unattributed. Observed RSS ranges from 1,071,324
to 1,170,748 KiB across the ten cycles; neither RSS nor driver usage is a rooted allocation total.

Directed final-run observations are 68.2304 ms for scrub-to-widget-paint, 108.811 µs for selection
feedback, 24.0927 ms for held drag, 10.5983 ms for committed drag and 11.2621 ms for project
replacement during fill. All identity/geometry checks pass. These are individual observations,
not a latency distribution or a controlled before/after comparison with the earlier width study.

## Width selection

The same pre-batching binary, generated source set and diagnostic configuration ran width one and
width two. This comparison selects the admission width; the final batched implementation receives
its own source-aligned width-one stress run below.
The filesystem was warm; this is neither cold network throughput nor an application-wide latency
distribution. Fill is the median of ten 100-frame fills. Each interaction value below is one
directed observation, with input-to-widget-paint as the pixel endpoint.

| Measurement | Width one | Width two |
| --- | ---: | ---: |
| Median fill | 6,325.71 ms | 3,130.72 ms |
| Maximum observed host claims, bytes | 1,049,361,376 | 1,891,344,288 |
| Maximum observed device claims, bytes | 1,257,120,784 | 2,397,971,472 |
| Maximum observed admitted phases | 1 | 2 |
| Scrub during fill | 71.89 ms | 94.79 ms |
| Selection event feedback | 101.67 µs | 111.41 µs |
| Held drag during fill | 38.31 ms | 37.50 ms |
| Committed drag | 9.41 ms | 59.78 ms |
| Project replacement during refill | 31.58 ms | 27.74 ms |

Width two improves fill throughput but worsens scrub/commit latency and consumes substantially more
reservation headroom. It does not pass the combined throughput/interaction/headroom gate. The
production service-backed RAM preview therefore defaults to width one. The existing CPU worker
pipeline retains its previous default; an explicit width-two fixture remains available for future
paired qualification.

Claim/phase maxima above are maxima of periodic UI observations, not exact transient high-water
measurements. The settled ownership assertions and retained payload counts are exact.

## Stage timing

Source statistics distinguish capture I/O, SHA-256, metadata probing and image decode. Native
statistics distinguish upload/processing host submission from fence-observation delay. Final
readback/transfer, host publication and encoding have separate intervals. Fence-observation delay
includes queue execution and owner scheduling; it is not shader time.

Optional real device timestamps cover an outer graph or transfer queue span and include queue idle
and submit gaps. Cross-submission comparison requires an enabled advertised calibrated-timestamp
extension, a valid timestamp period and valid-bit wrap handling. Unsupported devices report
Unsupported without allocating query resources. Results publish only after positive fence proof;
Busy, cancelled and uncertain results never become a valid duration. Driver/VMA diagnostics are
opt-in, sampled on the native owner at no more than one hertz and published as neutral snapshots.

The competing real EXR export in the final stress run records the following first-evaluation
intervals. This is one observed frame, with no claim about a cold operating-system page cache.

| Stage | Observed time |
| --- | ---: |
| Source capture I/O | 0.015560 ms |
| Source SHA-256 | 0.202263 ms |
| Metadata probe | 0.052850 ms |
| Image decode | 32.279333 ms |
| Upload host submission | 9.543840 ms |
| Upload fence observation | 2.110815 ms |
| Processing host submission | 4.048807 ms |
| Processing fence observation | 1.057102 ms |
| Final transfer | 8.280845 ms |
| Final host publication | 7.367256 ms |
| Calibrated outer graph queue span | 16.916608 ms |

The GPU-clock span includes idle/submission gaps and overlaps host intervals; these values must not
be summed into a shader duration. Two completed exports perform two final readbacks in total.
Their aggregate evaluation is 125.547 ms; the separate encoding-and-file-I/O boundary is 84.071 ms.
That last measurement includes file writing and is not a codec-only timing.

### Completed-step continuation baseline

The production executor has a default-enabled bounded same-turn continuation. Setting
`continueCompletedStepImmediately=false` reproduces the prior one-step-per-owner-turn schedule
using the same implementation. This comparison explicitly sets `enableNativeBatches=false` in both
modes, keeping command recording/submission unchanged. The fixed source-free authored
two-solid/fractional-translation/source-over scene runs at 720p and 1080p with capacity-one rooted admission, five warmups and ten measured paired
samples per mode, alternating order. Every content-cache-cold output matches the CPU reference and
the paired GPU output is byte-identical. Retained pipelines are warm; content caches are cleared.

| Extent | Controlled pending-poll delay | One-step cold median | Same-turn cold median | Genuine queue span, before → after |
| --- | --- | --- | --- | --- |
| 1280×720 | yield | 1.504 ms | 1.504 ms | 1.437 → 1.438 ms |
| 1920×1080 | yield | 4.639 ms | 4.552 ms | 4.589 → 4.443 ms |
| 1280×720 | 2 ms | 30.433 ms | 18.178 ms | 30.386 → 18.150 ms |
| 1920×1080 | 2 ms | 33.136 ms | 20.985 ms | 33.054 → 20.900 ms |

The controlled 2 ms delay matches the service's pending interval; live notifications may wake it
sooner. Six same-turn continuations remove about 12.32 ms of directly observed completed-step
owner-turn gaps. Tight-yield results show no consistent speedup. Both schedules still execute eight
actual kernels (the covered-solid producer and fill count separately): this qualifies removal of
polling gaps, **not multi-operation command batching**.
GPU clock spans include queue idle and host submission/polling gaps; fence observation is host wall
time. CPU preparation medians were 0.114/0.176 ms and uncached CPU evaluation 10.624/24.975 ms for
720p/1080p. No application FPS claim follows from these executor boundaries.

A separate observation pass keeps VMA traversal and driver sampling outside the timing medians.
Actual VMA payload peaks remain 34,233,520/77,607,120 bytes for 720p/1080p, matching root-owned native
payload; live image peaks remain 21,921,040/46,988,176 bytes. Both polling schedules return VMA
payload and root host/device claims to zero. VMA retains 100,663,296 bytes of blocks afterward;
blocks, payload and overlapping quota reservations are distinct. The terminal parity readback and
query-result retrieval also occur outside timed pixel publication. The normal and fault executor
suites pass after the switch. Reproduce with `bloom_runtime_gpu_scene_executor_test --require-device
--benchmark` and an explicit loader when needed; tests use the isolated display environment.

### Bounded native command batches

`enableNativeBatches=true` is the production default. Two to eight consecutive eligible solid,
translation, affine and source-over kernels record into one command buffer, with write/read
barriers and one completion fence. Other required kernels delimit runs and retain their native
paths. Every actual image, status/metadata/staging buffer and translation-axis host allocation is
covered before use. Axis scratch has fresh additive host admission through the same tracked
allocator in both batch and serial native paths; batch refusal cannot bypass that admission.

The comparison uses one compiled integer-grid raster merge, a fractional rotated affine placement,
and its composited output through the same production executor. Both modes retain same-turn
continuation and use a controlled 2 ms pending-poll interval. Five warmups precede ten measured
paired samples, alternating order, with content caches cleared and pipelines retained. Every output
matches the uncached CPU oracle to the existing 2e-6 gate and serial/batch GPU pixels are byte-exact.

| Extent | Actual kernels, both modes | Work submissions, serial → batch | Cold wall median | Genuine queue span |
| --- | --- | --- | --- | --- |
| 1280×720 | 11 | 11 → 2 | 23.497 → 4.832 ms | 23.431 → 4.797 ms |
| 1920×1080 | 11 | 11 → 2 | 23.036 → 4.458 ms | 22.987 → 4.348 ms |

Optional timestamp-marker submissions are separate from these work-submission counts. Queue spans
include host submission and polling gaps, and have ten valid calibrated GPU-clock samples per mode.
Executor planning medians are 0.008/0.009 ms at 720p and 0.009/0.009 ms at 1080p. Host submit medians
are 0.856/0.676 ms and 0.383/0.262 ms; fence-observation medians are 22.582/4.113 ms and
22.584/4.111 ms. Final parity readback, timestamp retrieval and memory traversal remain outside timed
pixel publication. These measurements establish fewer production work submissions and reduced
controlled scheduling gaps; they do not establish application FPS.

Batches retain their intermediate images until the shared fence. Observed actual VMA payload peaks
increase from 46,767,376 to 52,403,488 bytes at 720p and from 109,976,464 to 122,608,544 bytes at
1080p, matching root-owned native bytes. VMA block peaks are unchanged between modes at 134,217,728
and 203,161,600 bytes respectively. Settled payload and root host/device claims return to zero;
retained blocks are 67,108,864/167,772,160 bytes. A measured serial-memory limit refuses oversized
batch construction before submit and completes through the serial native path under the original
phase. Unchanged warm graphs submit neither work nor timing markers. Edited cached-media fixtures
also prove downstream batching with exact Ready external image references and retained source reuse.

Native entry gates validate topological references, device generation, finite parameters and checked
offsets. Proofs cover status rejection before any output publication, image and buffer construction
failures with backing-first same-origin prefix restoration, Busy-owner release/retry,
completion/cancellation cycles, owner-only getters, foreign release, unknown fence ownership and
genuine-proof device-loss retirement. A test-only uncertain
result after actual queue acceptance retains the exact owner, then discards every output after real
fence proof and recovers capacity-one admission without another job. The direct, normal and fault
executor proofs pass with batches enabled. The same `--benchmark --require-device` command above
reproduces both comparisons; the portable stub reports Unsupported.

## Source revision and refresh

Still-image hashing, probing and decoding consume the same bounded immutable encoded bytes.
Retained interactive revisions are looked up before any filesystem resolution, hash or decode;
warm retained work reports zero repeated source work. Linux video uses a bounded anonymous
disk-backed immutable snapshot and an owning inherited input capability, so the media worker
decodes the captured bytes even if the original pathname is replaced or removed.

Revalidate remains the default. Final rendering explicitly revalidates even if handed a copied
Retained request, and rejects unavailable or changed media rather than exporting a preview
placeholder. Relink identity, source digest, locator/base changes and explicit media purge cannot
silently reuse old source authority. A generation guard prevents old in-flight captures repopulating
the new source generation. Media purge also drops completed preview frames under the producer gate;
otherwise a finished-frame hit could hide the refreshed source.

See [media I/O](../architecture/media-io.md) and
[evaluation ownership](../architecture/evaluation-primitives.md) for the exact source, quota and
off-UI retirement contracts.

A private localhost SSHFS fixture exercises an actual disconnected FUSE mount: it establishes a
fresh key-authenticated read-only mount, interrupts only its private transport/client, and observes
kernel `ENOTCONN`. Sixty retained still/video warm requests perform zero repeated source read/hash/
probe/decode and preserve exact prepared-upload backing. The real media worker also decodes an
uncached video frame from its anonymous snapshot during the outage. Fresh/final requests refuse
unavailable sources. Reconnection and same-path replacement reject old strong digests; adopting the
new revisions restores final preparation. Cancellation, refresh-generation rejection and final disk
quota release pass. Cleanup removes the exact private mount and temporary keys. This is SSHFS
outage/reconnect evidence; it does not qualify SMB behavior or cold network throughput.

## LeakSanitizer attribution

The no-Bloom raw loader probe is unsuppressed. Loader-only initialization exits cleanly; creating a
native instance/device reproduces the same six-allocation libdbus family as the original host
finding. Matching module/build-id evidence identifies the unresolved callers inside the native EGL
driver. A current pinned EGL two-cycle raw probe and the correctly staged historical scoped host
both reproduce 3,308 bytes in six allocations. The historical 2,468-byte observation is retained;
changed DBus receive-buffer sizes account for the different total. Default EGL discovery also has a
separate Mesa/libdrm residual and is not substituted for the Wayland result.

This resolves attribution to an external driver/DBus initialization path without a blanket
suppression or application shutdown workaround. It does not claim a clean whole-native-host LSAN
exit. The [symbolized attribution record](../architecture/gpu-backend.md#native-loader-leaksanitizer-attribution)
and `tools/quality/native_loader_lsan_probe.cpp` preserve reproduction and limits.

## Integration and platform limits

Local Linux qualification passes the ten required-device ownership/compute suites, all six genuine
production route writers and the native coverage acceptance gate. Final packed/general service,
source-context, final-output, cancellation, unknown-completion and capacity-one recovery checks
pass. Full-resolution 100-frame reuse also passes; sustained playback above is the Half-resolution
fixture. The explicit Vulkan-OFF fallback gate passes all sixteen tests, with additional final
source/bridge regressions and targeted repair reruns. Native mode against that stub reports NotRun
and fails, including without an explicit requirement flag.

Strict analysis covers 384 changed-source/header translation units: 380 pass the initial final
sweep, and four test-only optional/type repairs pass exact reruns. Repository quality, formatting,
architecture boundaries, workflow validation and independent ownership review pass.

Hosted Linux Clang and Qt checks are required before integration; the commit-linked workflow
results identify the qualified revision. Hosted CPU checks do not qualify native hardware. Native
Windows/macOS execution and their platform qualification remain outside this owner-directed Linux
run, with explicit supported CPU fallback and no cross-platform GPU claim.
