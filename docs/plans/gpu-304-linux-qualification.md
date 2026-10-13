# Issue 304: Linux implementation and qualification

Status: integration in progress

Updated: 2026-10-13

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

## Width selection

The same binary, generated source set and diagnostic configuration ran width one and width two.
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

### Completed-step continuation baseline

The production executor has a default-enabled bounded same-turn continuation. Setting
`continueCompletedStepImmediately=false` reproduces the prior one-step-per-owner-turn schedule
using the same implementation. The fixed source-free two-solid/fractional-translation/source-over
scene runs at 720p and 1080p with capacity-one rooted admission, five warmups and ten measured paired
samples per mode, alternating order. Every content-cache-cold output matches the CPU reference and
the paired GPU output is byte-identical. Retained pipelines are warm; content caches are cleared.

| Extent | Controlled pending-poll delay | One-step cold median | Same-turn cold median | Genuine queue span, before → after |
| --- | --- | --- | --- | --- |
| 1280×720 | yield | 1.620 ms | 1.747 ms | 1.570 → 1.656 ms |
| 1920×1080 | yield | 4.632 ms | 4.525 ms | 4.553 → 4.443 ms |
| 1280×720 | 2 ms | 30.937 ms | 18.462 ms | 30.876 → 18.405 ms |
| 1920×1080 | 2 ms | 33.837 ms | 21.305 ms | 33.798 → 21.226 ms |

The controlled 2 ms delay matches the service's pending interval; live notifications may wake it
sooner. Six same-turn continuations remove about 12.32 ms of directly observed completed-step
owner-turn gaps. Tight-yield results show no consistent speedup. Both schedules still execute seven
native dispatches: this qualifies removal of polling gaps, **not multi-operation command batching**.
GPU clock spans include queue idle and host submission/polling gaps; fence observation is host wall
time. CPU preparation medians were 0.131/0.185 ms and uncached CPU evaluation 11.824/27.370 ms for
720p/1080p. No application FPS claim follows from these executor boundaries.

A separate observation pass keeps VMA traversal and driver sampling outside the timing medians.
Actual VMA payload peaks remain 34,233,520/77,607,120 bytes for 720p/1080p, matching root-owned native
payload; live image peaks remain 21,921,040/46,988,176 bytes. Both polling schedules return VMA
payload and root host/device claims to zero. VMA retains 100,663,296 bytes of blocks afterward;
blocks, payload and overlapping quota reservations are distinct. The terminal parity readback and
query-result retrieval also occur outside timed pixel publication. The normal and fault executor
suites pass after the switch. Reproduce with `bloom_runtime_gpu_scene_executor_test --require-device
--benchmark` and an explicit loader when needed; tests use the isolated display environment.

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

## Integration still to complete

Final source-aligned production preview/still/range/video/headless route reruns, immutable-video
closure, final strict/quality checks, and hosted Linux CI
remain integration gates. Focused CPU fallback requires an explicit Vulkan-OFF build. A missing
native device produces NotRun/Unsupported, and a required-device run fails rather than labelling a
CPU frame as GPU. Hosted CPU checks do not qualify native hardware.
