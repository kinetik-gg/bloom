# GPU reliability, media preparation, and preview execution plan

Status: working — implementation resumed after G2E; architecture choices remain gated by evidence

Updated: 2026-10-09

## Execution checkpoint

The owner resumed execution after the G2E checkpoint. G3B outline publication and displayed-intent
slices and G3A retention truth are accepted within the limits below. The immediate next G3B follow-up
is Viewer resize recovery and clipping. The narrow resize acknowledgement/visible-status repair is
accepted below; the pixel containment defect remains unresolved and G3 as a whole is incomplete.

Implementation proceeds sequentially from `e1086c13`. Each slice requires source review, focused
regressions, and the applicable native/product evidence before its successor is accepted. A passing
unit test does not complete an experience gate. A bounded final GPU-packed transfer into QWidget
is now authorized for the G3B layout repair, with its service reviewed before Viewer integration.
The broader host tier and residency policy remain separate decisions under G9.

| Package | State | Evidence |
| --- | --- | --- |
| G0 | Initial instrumentation slice accepted; remaining probes attach to their owning packages | Source review, quality checks, three focused suites, native acknowledgement correlation and original interaction fixture passed |
| G1 | Accepted: native validity and mapped-memory visibility | Source review, quality/lint, real operator/display/viewer/RAM/final-route parity and validation; independent Composite/viewer reruns passed |
| G2 | Initialization cleanup, metadata lifetime, image reclamation, vector quarantine, oracle recovery, and G2E cancellation/teardown accepted as bounded slices | Native parity/lifecycle/viewer/final paths and independent retirement/transition reruns have zero validation errors; aggregate accounting remains G6 work |
| G3B1 | Coherent outline publication accepted as a bounded slice | Native state/interaction regressions, independent compositor samples, CPU regressions and changed-source static analysis pass; G3B2A/B checkpoints follow |
| G3B2A | Clear/discard synchronization and bounded owner WSI progress accepted as a bounded slice | Independent source review and isolated GL native checks, isolated Vulkan lifecycle/progress, CPU admission/fairness tests, Vulkan-disabled render build and changed-source static analysis pass |
| G3B2B | Actual displayed intent, safe CPU cover and coupled hit mapping accepted as a bounded slice | Independent private compositor pixels, strict native interaction/project replacement, focused regressions, source quality and 58 affected translation units of static analysis pass |
| G3A | Accepted as a bounded retention/capacity slice | Independent source/artifact review, typed/live coverage regressions, measured 100-frame Full/Half native parity/warm replay, UI goldens, quality and 57 affected translation units of static analysis pass |
| G3B resize lifecycle/status | Accepted as a narrow slice; containment remains unresolved | Independent resize completion sequence, exposed previous pixels described as Stale, native/focused/UI checks and 20 affected translation units of static analysis pass |
| G3B packed service foundation | Accepted as a bounded core slice | Genuine offscreen GPU final transfers, exact provenance/retirement, Vulkan-disabled core build, quality and 45 affected translation units pass |
| G3B resize containment | Pending; immediate next slice | External pixels reject native viewport and geometry-admission approaches; staged asynchronous final GPU-packed QWidget output must qualify actual layout transitions |
| G4–G10 | Pending | No implementation or qualification claim |

G3B1 checkpoint (2026-10-09), accepted after independent source review and focused reruns: coherent required-outline publication and exact
owner-applied metadata are implemented as a bounded slice. Deterministic delayed raster, changing
geometry progress, semantic reuse, removal, late lease expiry, and owner refusal/recovery checks
pass; the original controller/interaction/project-swap, presentation, resident cache/service and
focused CPU regressions pass with genuine core/synchronization validation on the native paths.
Independent compositor captures retain a complete pair while replacement raster is delayed, then
show the complete replacement and explicit outline removal. A real selected-solid native drag in
a solid/text composition produced 73 sampled captures with no sampled outline loss (about 149 ms
median and 455 ms maximum sample gap). These samples do not establish per-refresh continuity,
playback performance, or text-specific dragging. G3B2 refusal/CPU-cover intent, failed/non-neutral
adjustment, global status truth and coupled applied hit mapping/bounds were still pending at that
checkpoint; G3B2B below qualifies them.
The clear/discard acquire-wait/layout dependency is addressed by the G3B2A checkpoint below.
G3A/G4 cadence is unchanged. Further native
GUI fixtures must use an isolated compositor; the user's desktop is no longer a test surface.

G3B2A checkpoint (2026-10-09), accepted after independent source review and native reruns: the clear/discard initial transition now shares the
acquire wait's transfer stage, and present-extension scalar storage lives through the API call.
Measured zero-timeout and 1 ns acquisition stalled while 1 ms enabled client release-event progress.
The final explicit owner-only probe requests at most 1 ms once per pump, rotates eligible targets,
and admits no fresh probe after close/resize/retirement/shutdown. Temporary results remain pending;
there is no fake acknowledgement, UI wait, or hard wall-clock promise. Isolated native raw-target
clear/image reuse, actual resize and acquired-unpresented retirement pass twice; eight simultaneous
three-target reuse rounds pass with genuine core and synchronization validation. Independent raw-target
and coordinator reruns also pass on the isolated GL renderer. CPU contract checks
cover the one-shot budget, fair rotation and admission gates. Affected native and portable-stub static
analysis, source quality/format checks, and the Vulkan-disabled render build pass. The G3B2B
checkpoint below handles visible intent, adjusted-display failure and coupled hit mapping; Qt
present-hook integration remains G4D work. No further native fixture uses the desktop; current
checks use the private isolated compositor.

G3B2B checkpoint (2026-10-09), accepted after independent review and private native/pixel checks:
desired controls, prepared candidates and actual native/CPU/cover displays now have separate
identity and currentness. Native metadata is correlated with the real owner acknowledgement;
painted CPU and cover metadata describes the exact captured source, geometry and intent. Status
and probes follow the active visible Viewer, with last-active/stable-visible fallback and explicit
no-viewer clearing. Passive picking uses the actual frame and destination; an active gesture keeps
its frozen mapping, while genuine domain/time/geometry/lifecycle changes cancel it. A synchronous
selection callback that invalidates a press cannot start a gesture or create a command.

Failed or held adjustment retains the prior complete display visibly stale. Failure reports a
concrete diagnostic without an unchanged retry loop. Unsafe refusal, domain replacement, resize and blank capture
require a safe CPU cover or explicit unavailable display. Explicit document rebind rejects old
local frames/tasks even when numeric IDs collide. The public raster cover owns its backing store:
a mapped alpha child conceals by transparency and reveals opaque captured pixels; a temporary
first-attach surface lasts only until genuine owner application. Fresh native targets re-establish
the cover after CPU retirement. No extra GPU parent commit, UI evaluation/readback, coverage exception,
precision change or scheduler-cadence change was added.

The final genuine native interaction proof passes all 17 stable capture phases with core and
synchronization validation clean. Independent pixel masks prove four exact text-outline moves and
undo, unchanged neutral pixels during held/failed EV, and matching adjusted-CPU/unsafe-cover image
geometry. Controller samples retain the exact outline until replacement/removal and the complete
320 × 240 refusal cover. Held first-attach samples fill four 160 × 120 quadrants from a DPR2
pixmap. Strict native project replacement, six focused portable suites, source quality and all
58 affected translation units of static analysis pass. Tests use private/offscreen surfaces only.
These samples do not qualify every refresh, sustained playback, desktop responsiveness, physical
monitor DPR2, or global keyboard focus. Linux native execution is locally verified; macOS/Windows
retain the explicit CPU fallback. G3A cache truth, G4 completion/cadence and public present hooks,
G6 aggregate memory accounting and G7 processing remain pending.

G3A checkpoint (2026-10-09), accepted after independent source, fixture and artifact review:
cache insertion reports actual retention, budget refusal, stale admission and allocation failure.
Allocating work precedes qualification/revision pruning; failed allocation and pre-admission refusal
preserve prior valid entries and charges. RAM preview separates requested frames, historical
successful preparations, live exact-grid retention and the longest contiguous half-open span.
For example, four preparations in a three-frame cache retain indices `{1,2,3}`, not all four.
Only a still-current fully retained range autoplays; partial results preserve useful samples for
ordinary manual playback. Expired leases refresh at a cache-owned 250 ms interval while residents
exist, followed by the existing 50 ms coalesced notification. Coverage is projected from retained
entries at mutation/result boundaries, not by scanning composition duration during paint.

Five focused portable suites, main-window/Viewer/playback checks, whole-window DPR1/DPR1.5 goldens,
repository quality and all 57 affected translation units of static analysis pass. Three fixture/type
warnings were repaired without changing production behavior or pixel assertions; their affected
translation units and changed RAM regression were rerun.

The resident count policy now uses finite lazy metadata admission (3584 cache slots within the
existing 4096 lease ceiling), with actual native bytes and device clamps authoritative. It no
longer limits a 1 GiB GPU pool to 51 frames by assuming each display image occupies 8 MiB.
An actual production sequence of 100 distinct 1920×1080 EXR frames retained all 100 Half products
at 960×540: 2,458,736 bytes per retained product, 245,873,600 bytes total under a 429,496,730-byte
cache share. Full products charge 8,848,496 bytes each. With the same 1 GiB pool, 49 preparations
leave 48 retained indices `{1..48}` and span `[0.04 s,1.96 s)`, correctly reporting partial budget
completion. A measured fitting 3 GiB pool retains all 100 Full products (884,849,600 bytes under
the 1,288,490,189-byte cache share). Each complete case verifies CPU-oracle pixel parity and 200
forward/reverse native presentations with no additional evaluation, scene dispatch or full-frame
readback. Presentation still performs native GPU work. Core and synchronization validation are
clean. Genuine expired admission combined with foreign eviction reports failure, not a budget
completion. Fixtures use private/offscreen surfaces and leave the owner's app session untouched.

These synthetic local uniform-colour EXR sequence checks use a private raw native target and
150 ms pacing between presentations. They qualify display dimensions, retention admission and warm
reuse; they do not establish TS010/heavy EXR/SMB throughput, actual playback-clock cadence,
every-refresh continuity or desktop responsiveness. RGBA32F working
precision and RGBA8 display storage are unchanged. Linux native execution is locally verified;
macOS/Windows retain explicit CPU fallback. G4 cadence, G5 media workload and G6 aggregate memory
accounting remain separate packages; G7 display ownership, G9 cache-tier decisions and G10 sustained
workload/platform qualification are also unchanged.

The owner subsequently reported `resize refused: the target is not Active or retire is pending`,
image spill into neighboring panels and blank Viewer areas at some sizes/positions, while status
still claimed GPU resident/current. This is the immediate G3B follow-up. The earlier settled/static
G3B2B captures do not qualify repeated resize/reposition during non-Active native transitions. The
next gate must externally prove current visible pixels and neighboring-panel clipping after layout
changes, including held attach/resize/retirement. The running old app and unsaved session remain
preserved; no overnight relaunch is part of this checkpoint.

Narrow resize lifecycle/status checkpoint (2026-10-09), accepted after independent review:
`completedResizeSequence` records actual native recreation separately from image `appliedSequence`.
Newest attach/resize intent uses the existing bounded mailbox; old A acknowledgements cannot end
an A→B→C or A→B→A gate. Native availability is separate from current-domain retention: an exposed
old coherent publication remains descriptive as Stale during changed overlays or retirement,
while Current requires Active and the exact publication domain. Hidden or unexposed output cannot
advertise a displayed frame or fall through to CPU metadata occluded by a live native target.
Real coordinator/client correlation, private held-retirement pixels and existing overlay/intent
proofs pass with clean core/synchronization validation; focused/UI checks, unchanged DPR1/1.5
goldens, source quality and 20 affected translation units of static analysis pass.

This checkpoint **does not fix resize clipping**. The original held shrink spilled 197,225 pixels
into a neighboring sentinel. A dedicated native QWidget viewport promoted editor ancestry and
produced blank/stale output, so it was removed. Harness-only retirement-before-layout admission
contained authored splitter changes, but an actual top-level shrink from 960 to 600 pixels left
89,125 GPU/checker/outline pixels outside the new window even with retirement requested first.
Neither experiment is installed in production. Their private raw captures remain evidence of the
unresolved boundary, not passing experience gates. The authorized next stage transfers only the
final GPU-prepared RGBA8 display asynchronously into budgeted host storage for ordinary QWidget
painting, with explicit GPU-prepared/CPU-painted provenance, no CPU scene re-evaluation, retained
native route coverage and measured warm cache reuse. Service lifetime/budget/parity review precedes
Viewer wiring. This bounded repair does not complete G3 or G4–G10.

Terminal task-input lifetime checkpoint (2026-10-09), accepted after independent review:
CPU and blocking-I/O task publication now releases the callable's captured inputs after execution
returns, or after queued removal. Cancellation of running work cannot destroy executing captures.
Six deterministic lifetime cases fail before the change and pass after it, covering completion,
queued cancellation and concurrent running cancellation on both executors. Typed results and
terminal metadata remain available. The three runtime suites, changed-source formatting and eight
runtime translation units of static analysis pass. No UI/app target was rebuilt; the seven unbuilt
G3A files remain separate. This is a bounded ownership repair, not completion of G6.

The owner reported stable outlines, then forced closure while playing animation in the accepted
G3B2B build. Kernel and service journals establish an 8 GiB launcher-cgroup OOM kill after 6m49s,
not an application exception. Default configured caches on the roughly 60 GiB host allowed about
30 GiB in total, exceeding that process cap; exact allocation ownership at the kill was not traced.
The task-capture defect is independently proven, not established as this incident's cause. Recovery
originals and journals were preserved. The same accepted app was relaunched with a temporary
configuration overlay setting operation and RAM-preview caches to 1 GiB each under the unchanged
8 GiB cap, with normal recovery/data locations and no memory-high throttle. A 420-second
nonmutating observation ended with the app alive, a 3.55 GiB service peak and no cap/OOM events.
The user's exact workload was not measured; this does not establish sustained playback or a plateau.
G3A resumes independently. G6 must account for service/container limits and result-mailbox ownership.

G0 adds opt-in bounded numeric frame tracing and a native Full-resolution 1920 × 1080 solid/text
fixture using four cold and four warm frame requests. The initial measured medians were 48.14 ms
and 32.01 ms respectively, through owner acknowledgement, with four warm RAM-cache hits, no CPU
fallback, and no full-frame readback. Device qualification precedes these samples. This small
sample does not establish sustained playback, pointer latency, unobscured pixels, or absence of
flicker. Worker-stage, source-I/O, surface visibility, and pointer probes remain required in the
packages that change those paths.

The same viewer fixture originally passed its functional assertions while reporting 41
core/synchronization validation errors; after G1 it reports zero. G1 includes extension dependency
negotiation, the swapchain acquire-to-render-pass dependency, resident transfer usage, mapped
visibility, and final/oracle readback barriers. Real RAM fill/replay retained four frames with zero
warm dispatches, and a mixed media/text 6000 × 4000 final-render fixture preserved CPU parity.
Mapped-memory tests observe real VMA calls and injected failures, but local allocations were
coherent: noncoherent execution and other platforms remain unqualified. See the owning
[backend checkpoint](../architecture/gpu-backend.md) for precise limits.

A separate resolution-transition attempt exposed cancellation recovery and failed-teardown
behavior for G2/G4; fixed-resolution success does not qualify that transition. G1 does not establish
flicker-free playback or repair the remaining scheduling, retention, and memory-ownership findings.

The user subsequently clarified the open G1 build's remaining manipulation symptom: dragging is
responsive, but object outlines and outer bounds blink in and out, resembling refresh lag. This is
a failed overlay experience check after the native validity repair; it does not establish slow
dragging or flickering image pixels. Prioritize G3B overlay publication and visible presentation
after G2E, then G3A retention truth. The causal mechanism remains to be instrumented.

The audit observations below describe the baseline; subsequent fixes and measured results belong
in this checkpoint and the owning architecture documents.

G3B packed-transfer Stage 1 checkpoint (2026-10-09), accepted after independent core review:
genuine GPU scene and selected Neutral/general display processing produce sealed immutable RGBA8
host products without WSI or CPU scene re-evaluation. Owner-thread asynchronous copy/fence polling,
off-UI materialization, exact device/owner binding, ROI geometry, overflow-safe metadata charging,
queued newest/cancellation and shutdown quarantine qualify. Full 1920×1080 measured source
8,847,360 B, staging 8,294,400 B and host 8,294,400 B; Half physically reduces extent and charges.
Shutdown preserves actual source/staging 17,141,760 B with host zero through service/Core/scheduler
destruction, separately from its conservative admitted ceiling. Neutral/general Full, Half, ROI
and adjustment parity, genuine native General regression, zero CPU fallback, 200 byte/proof
restamps without new preparation/dispatch/transfer, Vulkan-disabled runtime build, required source
quality and 45 affected translation units of static analysis pass. Initial fixture failures remain
preserved and do not qualify behavior.

This accepts the service foundation only. NativeResident remains the default; actual Viewer
GPU-prepared/CPU-painted adoption, external panel/top-level clipping, off-UI channel derivatives,
host-only cache retention, ROI placement and warm 100-frame widget replay remain Stage 2 gates.
The [existing ADR](../decisions/0006-gpu-backend.md) records the bounded presentation revisit.
Core aliases are not playback evidence; Linux controlled pixels are not sustained or cross-platform
qualification. G3 remains incomplete. G4–G10 and G6 service-aware headroom/unconsumed-mailbox
ownership follow-ups remain unchanged.

## Decision to make

Keep the CPU reference, typed scene vocabulary, semantic cache keys, GPU coverage contract, and
owner-thread resource discipline. Rework the execution and publication paths around them. Do not
start by adding more kernels, increasing cache limits, or replacing Vulkan with another API.

The implementation contains real GPU processing, but its surrounding machinery can dominate it:
repeated media reads before cache lookup, CPU waits between individual GPU operations, UI-timer
driven range filling, competing memory owners, and statuses that describe prepared work rather
than the pixels actually presented. There are also native validity and retirement defects that
must precede performance work. Passing pixel-oracle tests does not establish Vulkan validity,
responsive interaction, sustained playback, or bounded process memory.

This proposal intentionally permits internal API changes. It does not authorize weakening the GPU
coverage contract, silently reducing final-render precision, inventing exceptions, or moving pixel
operations onto the CPU under GPU provenance.

## Evidence and limits

- Source audit baseline: `04ca9ef2`, the merged GPU implementation. Latest main checked during the
  audit was `e1086c13`; its subsequent Properties changes do not alter the cited GPU/cache paths.
- Three independent reviewers examined the backend, cache/RAM preview, and viewer. Findings below
  were reconciled with direct source inspection. They are **source-confirmed**, unless marked as a
  timing model, measurement, or proposed policy. They are not a claim that every failure was
  reproduced on the current device.
- The initial checkout had no application build or dependency prefix. A developer-system configure
  failed because OpenColorIO was absent; Vulkan headers/VMA were also absent. A separate latest-main
  build restored the repository's dependency superbuild, built the app/media worker, and launched
  `e1086c13` successfully on Wayland. A blank-project observation showed about 471 MiB of service
  memory; an 8 GiB cgroup limit contains the app and its children during user testing. These are
  startup observations, not sustained playback or GPU-memory measurements. No GPU code was fixed.
  The session later exited without a reported failure after about three minutes, with a 6.1 GiB
  service-memory peak. The user reports creating a composition, solid, text, and animation;
  viewer playback flickered/glitched, responsiveness was poor, and direct manipulation was painful.
  This is a **user-observed result on the latest build**, not an instrumented reproduction.
  No heavy footage is needed for that report. The exact frame/surface/allocation cause remains
  untraced; memory-high reclaim may also affect timing near the configured 6 GiB soft limit.
- Two user-supplied SMB EXR sequences were inspected read-only. Repository fixtures below use
  anonymous labels; private paths and footage must remain outside the repository.
- A bounded Python probe streamed three full files per sequence in 8 MiB chunks. It did not decode,
  hash, flush OS/server caches, or render. The observed reads were 93–220 ms for A and 550–593 ms
  for B. These are **file-read observations**, not cold-cache guarantees or Bloom frame times.
- No end-to-end FPS, leak-free playback, cross-platform GPU qualification, or improvement is claimed
  by this audit. Package G0 establishes the reproducible application baseline.
- Focused latest-main verification built and ran `bloom.ui.ram-preview`,
  `bloom.ui.ram-preview-pipeline`, and `bloom.ui.playback_controller` offscreen: all three passed.
  The executed tests explicitly accept reported count 4 / retained size 3 / completed=true, and
  a cached 25 fps transport advancing only to frame 1 after 200 ms. These passes confirm that the
  questioned policies are encoded in current tests; they do not qualify native playback performance.

| Private workload alias | Observed source | Frames | One RGBA32F frame | One packed RGBA8 frame | Packed full range |
| --- | --- | ---: | ---: | ---: | ---: |
| A | 1920 × 804, RGB half, ZIP | 100 | 23.55 MiB | 5.89 MiB | 0.575 GiB |
| B | 4608 × 3164, RGB half, PIZ | 23 | 222.47 MiB | 55.62 MiB | 1.249 GiB |

These are payload arithmetic, excluding allocator granularity, metadata, staging, intermediates,
driver allocation, and presentation surfaces. Keeping B's entire range as RGBA32F alone takes
about 5.00 GiB. A 24 fps test of B needs roughly 1.0 GB/s of compressed source delivery before
decode; the observed share reads cannot meet that uncached. Warm preview must avoid those reads.

## Findings

### F1 — Invalid native commands and incomplete mapped-memory visibility (P1)

[`gpu_resident_display.cpp`](../../src/render/vulkan/gpu_resident_display.cpp), lines 372–375,
creates the Neutral display input/output buffers with storage usage alone. Lines 469–471 and
548–549 then use them as transfer destination/source. The helper in
[`gpu_resident_display_resources.cpp`](../../src/render/vulkan/gpu_resident_display_resources.cpp),
lines 76–99, forwards those flags unchanged. Vulkan requires the corresponding transfer usages.
The same path combines a HOST_WRITE access mask with a TRANSFER-only source stage at lines
481–498. Driver tolerance is not a correctness guarantee.

Separately, [`gpu_composite_resources.cpp`](../../src/render/vulkan/gpu_composite_resources.cpp)
maps/copies initial data without a flush and does not require coherent memory. Its consumers include
Composite, Affine, and Blend. Status reads also need an audit for invalidation. Non-coherent memory
can expose stale parameters or status; this is a portability defect even if one local heap happens
to be coherent. See the [Vulkan copy requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html),
[reverse-copy requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBufferToImage.html),
and [VMA mapping contract](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html).

### F2 — Failure paths can release resources before safe retirement (P1)

[`gpu_composite_resources.cpp`](../../src/render/vulkan/gpu_composite_resources.cpp), lines
115–201, submits a staging copy, waits up to five seconds, then destroys staging and local command
resources even when the wait timed out. Translation's X/Y metadata allocations exercise this
branch. A timeout is not completion. The helper also blocks the shared owner, delaying preview,
presentation, and cancellation while it waits.

[`gpu_solid.cpp`](../../src/render/vulkan/gpu_solid.cpp), lines 140–149, and
[`gpu_image_upload.cpp`](../../src/render/vulkan/gpu_image_upload.cpp), lines 37–40, reset native
pools before their RAII children during initialization cleanup. The pinned Vulkan-Hpp children free
themselves through their saved pools. Repair and test late failures, not only normal destruction.

### F3 — The production executor serializes host progress around each operation (P1 for latency)

[`GpuSceneExecutor::poll`](../../src/runtime/gpu_scene_executor.cpp), lines 283–354, either starts
one operation or polls one existing operation. Completion returns without starting its dependent
step ([poll implementation](../../src/runtime/gpu_scene_executor_poll.cpp), lines 270–281).
The preview service waits 2 ms between active iterations
([service loop](../../src/runtime/gpu_preview_display_service.cpp), lines 18–50 and 125–159).
The export owner uses 1 ms sleeps ([export evaluator](../../src/runtime/gpu_process_frame.cpp),
lines 210–242).

**Timing model:** absent intervening notifications, N immediately finishing sequential operations
need roughly `(2N − 1) × 2 ms` of preview pump waits, plus other stages. This is not measured GPU
time or a universal lower bound: wakeups can shorten it, and native work can lengthen it. It explains
why a fast kernel does not establish a fast graph. The standalone executor benchmark busy-polls and
explicitly excludes viewer/presentation; it does not measure this production cadence.

### F4 — Source verification precedes reuse and repeatedly reads whole EXRs (P1 for media)

[`selectImageSource`](../../src/runtime/image_source.cpp), lines 66–79, calls `probeImage` before
prepared-upload lookup. [`probeImage`](../../src/media/image/image.cpp), lines 208–223, hashes the
whole file; `decodeImage`, lines 260–285, hashes again before decoding. The native raw branch in
[`gpu_media_preparation.cpp`](../../src/runtime/gpu_media_preparation.cpp), around lines 351–435,
also bypasses the converted disk-cache path and builds a full-resolution upload before GPU proxy
resampling. Half/Quarter output therefore does not imply proportional source-I/O/upload savings.

A **finished display-cache hit** can bypass preparation; this finding concerns requests that reach
scene preparation, including changed transforms and evicted display frames. Calling these requests
“warm GPU” without accounting for their source reads is misleading. Preserve content verification,
but move it to an explicit source-revision acquisition boundary rather than rereading the source on
every transform request.

### F5 — RAM-preview completion does not mean the range is retained (P1 for trust)

[`PreviewFrameCache::insert`](../../src/ui/preview_frame_cache.cpp), lines 159–270, returns `void`
and can reject the frame or evict entries. [`RamPreviewController::drainResults`](../../src/ui/ram_preview_controller.cpp),
lines 364–375, increments `cachedFrameCount_` regardless, and treats an eviction as successful
completion. [`ram_preview_tests.cpp`](../../src/ui/tests/ram_preview_tests.cpp), lines 679–684,
even expects a cached count of four while only three entries remain.

With an injected/effective zero budget, undersized budget, or allocation failure, preparation can
finish without a reusable
range. Counts and green coverage must describe live, servable entries, not cumulative successful
evaluations. A partial range is useful, but must be reported as partial.
The saved QSettings value zero selects defaults; use an injected/effective zero for this regression.

### F6 — Production completion delivery and playback policy work against throughput (P1)

The app constructs the default [`TaskUiBridge`](../../src/ui/include/bloom/ui/task_ui_bridge.hpp)
at [`main.cpp`](../../apps/bloom/main.cpp):161. Its interval is 100 ms. RAM/background controllers
collect/refill through `snapshotsPolled`; the two-slot RAM pipeline therefore has an idealized
ceiling around 20 newly completed fast frames/s when this is its only refill trigger. Foreground
preview has a separate fast timer, so the 100 ms claim must not be applied to every viewer result.
Several native tests inject a 1 ms bridge and bypass the product bottleneck.

[`PlaybackController`](../../src/ui/playback_controller.cpp):300 passes its 16 ms timer interval
as the preparation deadline, even at a 25 fps source period of 40 ms. The
[result consumer](../../src/ui/composition_preview_result.cpp):45–68 rejects a late delivery even
when it is still current. The [controller](../../src/ui/composition_preview_controller.cpp):731–735
cancels active playback work when the next request arrives. Slow cold work can be repeatedly
cancelled; valid 20 ms work can miss an unnecessarily short deadline.

The cached path also advances only one frame per tick when behind
([playback](../../src/ui/playback_controller.cpp):265–270). Zero reported drops can therefore
coexist with slower-than-real-time playback. Real-time/drop and play-every-frame must be separate,
visible policies, both measured against elapsed time.

### F7 — UI work and viewer analysis escape the accelerated path (P1/P2)

CPU preview-cache insertion strips the process image by copying the packed frame on the UI
completion path ([cache](../../src/ui/preview_frame_cache.cpp):24–36,236;
[frame product](../../src/runtime/prepared_preview_frame.cpp):114–123). CPU channel display also
copies/remaps a full image on the UI thread ([viewer](../../src/ui/viewer_editor.cpp):291–326).
At B's size, even a packed copy moves about 56 MiB. Falling back under memory pressure can add work
and transient allocation exactly when the application is least able to tolerate it.

Exposure/gamma uses [`submitViewerAnalysis`](../../src/ui/composition_preview_controller_analysis.cpp):49–57,
which submits the CPU preparation function directly instead of the GPU submitter. The resident
service already supports general adjusted display requests. The viewer can retain neutral base
pixels while non-neutral controls remain set when adjusted preparation is pending/refused/failed
([viewer analysis](../../src/ui/viewer_editor_analysis.cpp):46–85). This requires publication
correctness as well as acceleration.

### F8 — “GPU resident / current” describes preparation, not what is on screen (P1 for trust)

[`window_status_bar.cpp`](../../src/ui/window_status_bar.cpp):86–116 derives provider/currentness
from the prepared controller frame. The viewer can paint a retained older CPU fallback while native
attachment/presentation is pending or refused ([viewer GPU integration](../../src/ui/viewer_editor_gpu.ipp):44–55;
[viewer paint](../../src/ui/viewer_editor.cpp):2347–2349). Worker-result identity checks are useful
and already present; they do not establish presentation truth.

Track desired, prepared, retained, submitted-to-present, and actually presented identities
separately. Bind status, probes, and selection overlays to the product actually visible. A queued
presentation acknowledgement must not be described as physical scanout unless the platform supplies
that evidence.

### F9 — Multiple budgets do not constitute one bounded application working set (P1)

Preview resolves a device pool and partitions it into lease/scene/request allowances
([capacity resolver](../../src/runtime/gpu_preview_resident_capacity_resolver.cpp),
[policy](../../src/runtime/gpu_preview_resident_capacity.cpp)). This is a useful boundary, but
does not cover the whole app:

- Export creates another device, allocator, scene cache, executor, and owner thread
  ([main](../../apps/bloom/main.cpp):292–310;
  [export owner](../../src/runtime/gpu_process_frame.cpp):348–375). Its default per-request budget
  comes from available **host** RAM divided by four, not a shared device reservation.
- [`gpu_memory_budget.hpp`](../../src/runtime/include/bloom/runtime/gpu_memory_budget.hpp) calls
  `ledger.allocate()` without overrides. That function computes constructor/default allocations
  ([ledger](../../src/runtime/memory_budget_ledger.cpp):251–285), rather than returning each
  registered cache's current pressure-adjusted assignment. Its “live assigned budget” comment is
  stronger than its implementation.
- The frame cache reports resident GPU bytes into a host-memory ledger
  ([cache](../../src/ui/preview_frame_cache.cpp):74–87), while prepared uploads, GPU caches,
  active pins, native scratch, pending retirement, and allocator blocks have different owners and
  lifetimes. On a discrete GPU, VRAM is not reclaimable host RAM. Unified-memory systems need an
  explicit shared-heap model instead of treating both counters as independently available.
- The real host stores [`GpuPreparedUploadCache`](../../src/runtime/gpu_prepared_upload_cache.cpp)
  and [`GpuSceneCoverageCache`](../../src/runtime/gpu_scene_coverage_cache.cpp) have local maxima
  but do not register with the host ledger. Host pressure can trim the visible caches while these
  stores keep retaining and admitting data outside the reported shared allowance.

Account physical allocations once, with separate host/device charges, and record every retention
reason. Reservations must cover preparation, queued work, in-flight work, pinned results, and
retirement, not only cache entries. Increasing the current 50/20/30 shares is not the repair.

### F10 — Qualification is narrower than the experience it is used to defend (P2)

Keep the exhaustive operation/route gate. Add a separate product acceptance layer. Native pixel
fixtures, sparse samples, small graphs, fast test polling, and zero-dispatch warm hits cannot prove
wall-clock playback, source I/O reuse, transient memory, or the actually shown frame under fallback.
The backend also allocates a fresh output for identity composition-output translations; remove that
only after exact descriptor/ROI/alpha equivalence is established. Foreign-thread image destruction
needs a bounded owner-reclaim path rather than invisible permanent retention. These belong in
lifetime and allocation tests, not merely kernel parity tests.

The latest-build user check reinforces the missing product gate: ordinary solid/text animation and
viewer manipulation were reported as glitchy, flickering, and unresponsive. Treat this as a first
priority acceptance failure. Source defects above are plausible contributors, not a proven causal
explanation of those particular flickers. Trace surface generation, CPU-cover visibility,
submitted/applied presentation sequence, displayed identity, and interaction override generation.

## Proposed execution model

1. **Source revisions:** immutable, verified source tickets own content identity and stable media
   access. A bounded I/O/decode pipeline produces shared raw frames. Live-file changes create a new
   ticket after validation; they never silently reuse stale pixels. A managed local content store is
   one candidate for SMB input. Stat-only trust is not an adequate replacement for digests.
2. **Scene planning:** compilation and semantic keys stay independent of device and request
   generation. Plan the live working set before admission; reuse immutable raw sources and
   unaffected subgraphs. Interactive overrides remain distinct from committed truth.
3. **GPU execution:** one app-owned device domain, explicit priorities and reservations shared by
   preview and export. Record bounded batches with GPU dependencies; avoid host completion between
   every operation. Use persistent pipelines and safe reusable allocation pools. Retire resources
   against actual completion serials. Preserve a separate native-present retirement proof.
4. **Caches:** independently account decoded host data, process-image reuse, display products,
   playback coverage, and in-flight/retiring bytes. Use semantic content keys for reuse and request
   identities for publication. Admission returns a typed outcome; eviction never pretends a range
   is fully cached. Cache placement should consider reuse, recomputation cost, and the playback
   window rather than retaining every intermediate indiscriminately.
5. **Viewer:** derive display intent from an existing process product where possible; exposure,
   gamma, channel, pan, zoom, and overlays do not automatically reevaluate/decode the composition.
   UI callbacks manipulate small immutable handles. Presentation state says exactly what is shown.

Vulkan can express GPU dependencies without a CPU wait per node; consult the
[Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).
Timeline semaphores are already a bootstrap requirement, but using them is not itself a speedup.
The implementation must demonstrate fewer host wakeups and submissions on the production route.

RAM preview should eventually have a host-memory tier and a smaller VRAM working window. That is a
separate design decision: the current resident-preview contract forbids full-frame readback. If a
bounded asynchronous transfer of a finished packed display frame is chosen for host spill, amend
that owning contract explicitly and add transfer accounting/parity tests. Keep the interactive
resident path at zero full-frame readbacks; never introduce per-node roundtrips or label CPU
rendering as GPU. Do not slip this semantic change into a cache bug fix.

## Work packages

Each package is an independently reviewable iteration. Start with a failing regression or recorded
baseline, preserve it as evidence, change the smallest owning boundary, then rerun that gate through
the product path. Split a package further if it cannot be reviewed coherently; do not combine all
of these into a GPU rewrite PR. Complexity indicates relative scope, not a calendar estimate.

| ID | Result | Depends on | Scope |
| --- | --- | --- | --- |
| G0 | Repeatable product trace and workload baseline | — | Medium |
| G1 | Valid native usage, barriers, mapped-memory visibility | —; use G0 where available | Small/medium |
| G2 | Safe failure/retirement and bounded owner reclamation | G1 | Medium |
| G3 | Truthful cache coverage and visible-frame state | G0 | Medium; split cache/viewer changes |
| G4 | Completion-driven fill and correct playback clocks | G0, G3 | Medium |
| G5 | Verified source revisions and shared raw-media reuse | G0 | Medium/large; two slices |
| G6 | Unified allocation accounting and reservations | G0, G2 | Large; ledger before policy |
| G7 | Shared display storage and per-viewer GPU derivation | G1, G3 | Medium; CPU ownership first |
| G8 | Bounded GPU batches and reusable resources | G1, G2, G6 | Large; one vertical graph first |
| G9 | Playback residency placement and optional host tier | G3, G5, G6, G8 | Separate design decision |
| G10 | Sustained workload and platform acceptance | Applicable packages above | Continuous, then release gate |

### G0 — Instrument the application route before changing its performance

**Own:** `apps/bloom/main.cpp`; runtime preview/scene services; `TaskUiBridge`; preview, RAM,
playback, and presenter controllers; a new opt-in benchmark harness under `tests` or `tools`.

**First slice:** record one correlated frame history using existing stage boundaries and counters:
source revision, semantic output key, request generation,
desired/prepared/retained/present-submitted/presented identities, actual CPU/GPU provider, fallback
reason, and timestamps for queue, source validation/read/decode, plan, upload, dispatch, display,
delivery, and present acknowledgement. Count bytes read, hashes, decodes, uploads, native submissions,
host polls, and cache admissions/rejections/evictions where already available. Start with one
cold/warm fixture and a bounded event buffer. Begin with a simple solid/text animation and direct
manipulation, then the sequence workloads. Do not block G1/G2 on comprehensive telemetry.
Add missing native timestamps/fault seams with G1/G2, publication probes with G3/G4, media counters
with G5, and the allocation inventory with G6. Distinguish CPU wall time from GPU time and
acknowledgement from scanout. Telemetry must not record private media paths in publishable artifacts.

Use actual composition-root defaults. Capture those defaults in results so the harness cannot
quietly use a 1 ms bridge where the app uses 100 ms. The owning iterations add deterministic injection
for delayed decode, 20/60/120 ms preparation, cache refusal, present refusal, and delayed retirement.

**Gate:** a single request can be traced to what the viewer actually shows; cold and warm runs are
separable; the same workload/script can be rerun at a named revision. A no-device run is explicit
NotRun/Skipped and `--require-device` fails. No performance thresholds are inferred from counters
alone. **Exclude:** renderer refactoring and default-budget changes.

### G1 — Repair Vulkan validity and memory visibility

**Own:** `src/render/vulkan/gpu_resident_display*`, `gpu_composite_resources.cpp`, and its
Composite/Affine/Blend consumers. Update tests in `src/render/tests` and the owning backend document.

Correct transfer usage and stage/access compatibility. Give mapped allocations one explicit
flush/invalidate contract, use the VMA helpers correctly, and audit all callers for status and
parameter visibility. Keep semantics and shader identities unchanged unless a shader actually
changes. Do not hide a validation message or force a heap type merely to avoid exercising the bug.

**Gate:** real Neutral/general display and affected operators pass Vulkan core and synchronization
validation with zero relevant errors; existing CPU parity remains unchanged. Exercise a
non-coherent memory type when available, otherwise report that device case as untested and retain
an instrumented test proving the required flush/invalidate calls and ranges. Verify default viewer,
RAM fill, and one final-render path. **Exclude:** batching, cache policy, and new operations.

### G2 — Unify submission lifetime and failure cleanup

**Own:** `gpu_composite_resources.cpp`, Solid/Upload initialization cleanup, `gpu_bounded_retirement.hpp`,
`gpu_image.cpp`, and the existing native fault seams.

Move metadata staging into an asynchronous submission lifetime that owns staging, destination,
command resources, and completion proof together. Cancel publication promptly, retain submitted
resources until proven retired, and account/quarantine unproved work explicitly. Destroy RAII
children before pools. Add a bounded owner reclamation mailbox for foreign-thread releases or make
the ownership violation explicit and diagnosable; never silently lose allocator/image handles.
Avoid a new global service locator or generic resource framework: extend the owning render types.

**Gate:** inject allocation failure at every initialization boundary, timeout after successful
submit, cancellation, device loss, and foreign-thread final release. Assert no referenced resource
is freed early, no invalid pool access, exactly one eventual reclaim/diagnosed quarantine, and
retained bytes remain charged. Presentation retirement uses its existing WSI proof, not a compute
fence substitute. **Exclude:** capacity increases and precision changes.

G2B local evidence (2026-10-08): Translation/PointResample parity, mapped initialization/status
instrumentation, partial-axis/status flush failures, actual-allocation budget refusal, cancellation,
unproved orphan/quarantine retention, exact eventual staging reclaim, and drained loss diagnostics
pass native tests. The original PointResample operation continues to report its exact quarantined
submission until owner polling proves retirement, preserving runtime admission charge. Resident
viewer and mixed media+CST+text final export also pass with zero core/synchronization validation
errors, checked separately from exit status. Supervisor source review and independent Composite,
PointResample fault/retirement, and Composite/Blend/Affine retirement reruns passed with the real
validation layer loaded and zero validation errors. Observed native mappings were coherent; noncoherent
hardware execution remains unqualified. No cross-platform GPU qualification is claimed.

G2C image-owner scope: each process/display image now reserves bounded owner-reclamation metadata
before native allocation, retains actual native allocation bytes through foreign final release and
owner destruction, and exposes closed-generation retention. Preview owner pumps and offline scene
polling reclaim only the exact allocator/thread lifetime. The device facade's destruction or move
replacement leaves surviving images valid; late foreign releases remain explicitly charged within
the same fixed process ceiling. Vector coverage quarantine truth, oracle readback recovery, and the
resolution/cancellation transition remain separate G2 work. Local native image reclamation,
Composite CPU parity, resident-display CPU oracle, shared primitive retirement, resident viewer,
and mixed final-render gates passed with the genuine core/synchronization validation layer loaded
and no reported errors. Supervisor source review and independent Composite and final image
reclamation reruns passed. Quality and affected-TU lint passed, including the final private-header
changes. Both family
retention-only unproved-flag tests leave exact source-proof recovery to the following oracle slice;
no unproved retirement or pixel execution is fabricated. Cross-platform GPU remains unqualified.

G2D1 local vector-coverage evidence (2026-10-08): the original failed producer preserves its
exact quarantine token and device identity, reports outstanding retirement, and retires only its own
submission through real owner fence polling. Destruction retains the existing pipeline/descriptor
resident slot until the quarantined job retires. Native primitive tests prove actual VMA allocation
count and bytes remain retained, cancellation publishes no mask, stale tokens cannot release newer
jobs, and an orphaned resident reclaims exactly once after proof. The production runtime vector
executor retains admission through unproved failure, refuses retry, then recovers genuine covered
fill CPU parity after owner retirement. PathCoverage, CoveredSolid, and executor fault/parity gates
passed with genuine core/synchronization validation and no reported validation errors. Supervisor
source review and independent PathCoverage/executor reruns also passed; quality and affected-TU
lint passed. The
historical quarantine fuse remains latched after recovery; current retirement is reported by exact
submission ownership. Oracle readback recovery and service cancellation/accounting remain separate
G2 work. No cross-platform GPU qualification or complete G2 acceptance is claimed.

G2D2 local oracle evidence (2026-10-08): full process/display and sparse display readbacks share
two bounded reservations, retain the exact source image and actual staging allocation bytes through
unknown completion, and recover only after the original owner's real native fence proof. Native
fixtures cover same-source retry without a separate pump, normal owner-pump recovery after foreign
final release, repeated timeout/retry, two concurrent owners, aggregate-capacity and actual-allocation
budget refusal, stale proof/slot/thread identity, and closed-owner retention. Oracle, image-owner,
process/output-colour final readback, resident display, and Composite parity gates passed locally
with genuine core/synchronization validation loaded and no reported validation errors. A missing
loader fails the required-device gate. Quality and affected-TU lint passed, including new private
files. Supervisor source review and independent oracle/Composite reruns passed with the genuine
validation layer loaded and no reported errors. Actual driver device-loss branches are handled
consistently; this evidence does not claim
that a device-loss event was induced. Service cancellation/accounting and remaining G2 transition
work are still outstanding; cross-platform GPU and complete G2 remain unqualified.

G2E local cancellation/teardown evidence (2026-10-08): actual EditorArea-owned Full, Half,
Quarter and Auto controls supersede real submitted solid/text work through default controller and
TaskUiBridge intervals. Neutral and General OCIO arms recover identity-matched native
acknowledgements and the policy's resolved dimensions without CPU fallback or full-frame readback.
The native gesture fixture changes resolution after press/move, continues native pointer input,
and uses Escape to restore document truth without a command. Submitted display failure retains its
active stage and valid completion until owner retirement, including concurrent supersession; a
typed unexpected scene poll failure remains diagnosed when cancellation races it. The General
native/runtime regression injects unknown completion after real submit, preserves original failure
and native program ownership through cancellation, retires only after real fence proof, and then
passes healthy retry against the unchanged CPU pixel oracle. The observation seams do not claim
an actual driver error or induced device loss.

Every ordinary assertion exit now shares bounded WSI retirement and controller/bridge/service
shutdown. An intentional failure after real native presentation exits normally with code 1, and a
dedicated controller-shutdown mode begins with genuinely submitted work. Production EditorArea
replacement also passes while scene work is submitted. The final fatal unproved-WSI path retains
the entire owning fixture until process termination; normal assertion failure does not take it.
These local native gates load real core/synchronization validation and report no validation errors.
This is correctness evidence in a shared environment, not sustained performance, cross-platform GPU
qualification, or qualification of the separately reported outline flicker. Historical family fuses
remain distinct from current submission ownership. The live service limit is 512 retirement pumps;
scheduler-charge transfer and aggregate native quarantine admission remain G6 work. Root
independently reran Neutral and General transitions and the intentional assertion-exit
fixture with real validation and zero errors. The additional root OCIO invocation used a mistaken
executable name and did not run; the agent’s corrected native OCIO parity/retirement run passed.
G2E is accepted as a bounded cancellation/teardown slice; complete G2 and the broader experience
gates are not claimed by this evidence alone.

### G3 — Make retention and presentation state truthful

**Slice A owns:** `PreviewFrameCache`, `RamPreviewController`, RAM state/UI/tests.
Return typed insertion results such as Retained, AlreadyPresent, RejectedBudget, RejectedStale,
AllocationFailure, plus retention/eviction changes. Report requested, prepared, currently retained,
and playable coverage independently. Count the actual time-key set, not successful calls. Partial
fill must stop/refill according to policy and never label the full requested range complete.

**Slice B owns:** viewer resident controller/presenter, preview state, status/probe/overlay code.
Add per-viewer presented identity/provider/display intent and exact acknowledgement sequence.
Preserve the last good image during replacement, explicitly stale. A failed adjusted display must
not claim current adjusted pixels while showing neutral pixels. Do not equate a live lease with a
visible frame or a queued present with a completed one.

**Gate:** zero cache budget, one-frame capacity, allocation failure, mid-fill eviction/purge,
project swap, resize, forced presentation refusal, and failed exposure adjustment all show correct
coverage/status. Use visibly distinct frames/intent markers and a pixel oracle to catch a stale
picture behind a correct counter. Request-generation restamping never alters semantic identity.
**Exclude:** changing playback cadence or adding a new cache tier.

### G4 — Separate completion delivery, transport, and background progress

**Own:** `TaskUiBridge`, scheduler completion notification seam, RAM/background pipeline,
`PlaybackController`, `CompositionPreviewController`, and their production-timing tests.

Deliver frame completions with a coalesced queued notification to the UI; keep coarse Jobs-panel
polling independent. Refill bounded RAM preparation on completion rather than on the monitor tick.
Use a lifetime-safe neutral completion subscription: workers/scheduler retain no widget or QObject
references, teardown invalidates the subscription, and queued wakes stay bounded after panel/session
replacement. Do not substitute a permanent 1 ms UI poll. Derive playback target and deadlines from rational
frame time and the monotonic/audio clock. Distinguish real-time/drop from explicitly frame-accurate
slower playback. Permit bounded useful cold work to finish for later reuse while prioritizing the
newest visible request; superseding the desired frame must not automatically destroy all useful
progress. Stop obsolete work at safe boundaries with bounded queue growth.

**Gate:** 24/25/30/60 fps timed tests, including >frame-period producers, cache pressure and RAM fill
running alongside interaction. At 25 fps a still-current 20 ms result is not rejected merely because
the transport timer is 16 ms. Slow work cannot cause endless cancellation with no reusable result.
Warm cached playback advances according to the selected policy; report achieved rate and drift as
well as drops. Fast fill has no 100 ms staircase. **Exclude:** GPU command recording changes.

### G5 — Remove repeated media I/O from warm scene preparation

**Slice A owns:** media selection/probe/decode, `GpuSceneMediaContext`, raw upload preparation,
existing decoded/disk caches, and asset-revision invalidation. Specify an immutable verified source
ticket that can be reused by CPU and GPU preparation. Coalesce concurrent requests for the same
source; share immutable decoded storage instead of recopying a full source. Decide explicitly how
external modifications, missing files, relink, network reconnection, and final-export validation
produce or invalidate tickets. A managed content-addressed local snapshot avoids a file changing
under an admitted render. Do not replace strong content identity with size/mtime alone.

**Slice B owns:** bounded sequence prefetch and proxy/disk-cache integration. Use the same verified
raw-source cache for GPU and CPU routes. Fetch ahead within the active playback window, cancel
irrelevant ranges, and account compressed I/O buffers and decompressed frames before admission.
Keep CPU decompression separate from required GPU working-space conversion. Any lower-resolution
media proxy must carry explicit quality/identity and preserve the final-render source contract.

**Gate:** a transform-only edit on a warmed unchanged source performs zero source-byte reads,
hashes, and decodes; a warmed resident source adds zero uploads. Cold work reads/verifies one stable
source revision without duplicate concurrent loads. Changed content cannot return old pixels under
a new ticket. Compare SMB and local inputs, Full/Half/Quarter, cold display-cache miss versus warm
scene-cache hit, interrupted reads, and file replacement. **Exclude:** unbounded prefetch or global
OS cache flushing.

### G6 — Account the entire working set before allocating it

**Slice A owns:** host ledger, GPU capacity policy/resolver, prepared-upload cache, scene cache,
lease registry, export evaluator, and status snapshots. Inventory actual allocation ownership and
replace default-policy lookups masquerading as live assignments. Distinguish logical bytes, actual
allocator charges, committed blocks, pinned bytes, queued/active scratch, and retiring/quarantined
bytes. Charge shared allocations once; expose retention reasons. Move pressure sampling/application
out of status-bar ownership into the application runtime; the UI observes it. Handle discrete and unified heaps
explicitly. Measure process/driver residuals rather than pretending counters equal RSS/VRAM use.

Include the remaining native display/OCIO/presentation quarantine owners in that inventory.
Their historical failure counters/fuses do not reserve space for already-created objects; the
presentation target's full quarantine array can also leave later retained objects untracked.
These source-confirmed gaps require ownership records and reservation transfer, not a larger counter
limit. Preserve the presentation engine's separate WSI retirement proof. The accepted G2 primitive,
image, and oracle slices do not establish aggregate accounting for these other owners.

The 2026-10-09 launcher incident adds two concrete audits. Derive effective process headroom from
service/container memory limits as well as host capacity, reserving room beyond configured caches;
host-only defaults exceeded the imposed 8 GiB process cap. Also distinguish result delivery owned
by live handles from unnecessary history retention: terminal `TaskRecord::work` still owns its
result mailbox, so an unconsumed result can remain until history eviction after consumer handles
are gone. The accepted capture-release checkpoint preserves this separate result policy. Neither
container-aware budgeting nor mailbox retirement is resolved by that checkpoint.

**Slice B owns:** an explicit application-owned reservation/budget coordinator passed to the
existing owners. Preview and export share the device allowance even before they share a device.
Refresh capacity/pressure, stop background admission first, preserve the visible frame and active
safety-critical pins, and shrink reusable retention. Grant request scratch from actual available
headroom; avoid rigid percentages that strand free memory while denying a runnable frame. Every
allocation path must release or transfer its reservation exactly once.

**Gate:** simultaneous preview/fill/export respects aggregate domain reservations; low user caps
and changing device pressure affect all owners; preparation queues cannot retain multiple
unaccounted full frames. Ten fill/purge/edit loops reach a bounded plateau. Purge reports pinned
and retiring residuals and eventually reclaims evictable data; RSS need not become zero because
allocator blocks/driver reservations may remain. No admission workaround weakens GPU coverage.
**Exclude:** changing canonical RGBA32F to half precision.

### G7 — Make display interaction a cheap derivation

**Slice A owns:** `PreparedPreviewFrame`, `PreviewDisplayOnlyFrame`, display-buffer storage,
CPU channel display, and `PreviewFrameCache`. Use immutable shared display storage so retaining a
CPU frame drops the process-image reference without copying packed pixels. Do any necessary
packing/remapping on a worker. Admit byte cost before allocation and bound queued publication.

**Slice B owns:** viewer analysis/adjustment and the resident general-display route. Route
exposure/gamma through the existing GPU submitter first; then reuse a process/scene result across
display intents. Keep per-viewer display/channel/ROI state separate where the UX calls for it.
Key overlays by geometry/style/viewport dependencies, not request generation alone. Pan/zoom and
channel changes should normally change presentation parameters without decode/scene evaluation.

**Gate:** current intent and shown pixels always agree or are explicitly stale/failed. CPU cache
insertion touches no full-frame pixel buffer on the UI thread. A warm GPU exposure/gamma change
adds display work only; pan/zoom/channel adds no source read/upload/scene dispatch when resolved
output resolution and ROI are unchanged. Auto-resolution changes are separately traced and tested.
Two viewers with different display intents remain independent. **Exclude:** a new color model or weakening the
CPU display oracle.

### G8 — Replace operation-at-a-time pumping with bounded GPU batches

**Own:** `GpuSceneExecutor`, native operation recording/resource helpers, preview/export service
integration. First implement one representative upload → color → transform → merge → display graph;
keep the existing operation vocabulary and CPU parity fixtures.

Record dependency barriers and multiple operations into bounded command batches. Retire using
completion serials/timeline values rather than a host sleep between every dependent kernel. Keep
finite batch size, cancellation/publication checks, and fairness between visible work, fill, and
export. Reuse pipelines, descriptor/command pools, and scratch after proven retirement. Elide
identity outputs only when descriptors, bounds, ROI, and alpha semantics are identical. Start with
one queue; extra async queues are a later measured decision, not a prerequisite.

Then consolidate preview/export device ownership if that simplifies sharing without starving
interaction. Preserve typed route entrypoints and one final codec-boundary readback. Multiple
in-flight batches require G6 reservations; they must not multiply resident memory invisibly.

**Gate:** production traces lose the per-operation 2 ms/1 ms host-wait staircase; command submission
count follows bounded batches rather than operation count for the representative graph. Warm
unchanged output remains zero scene dispatch. CPU/GPU image and color parity, cancellation,
retirement, cache identity, and every required render route pass. Demonstrate end-to-end gain and
peak-memory change, not just dispatch timing. **Exclude:** broad kernel fusion or a backend rewrite.

### G9 — Decide what RAM preview retains and where

**Own:** a focused ADR plus frame-cache/lease-registry/scene-cache admission. Decide whether the
product promises a VRAM-only preview window or a bounded RAM-backed range with VRAM promotion.
Present host memory and GPU memory separately. Use a playback-window policy and explicit pinning
instead of having scene intermediates compete blindly with finished display frames.
Reuse identical immutable display storage across timeline slots when semantic output and display
intent match; keep each slot's request/time provenance separate. Test a static range so the number
of timeline entries does not automatically imply that many unique full-frame allocations.

If host spill is adopted, revise the resident readback contract deliberately, transfer only finished
packed display products asynchronously, count every byte, and preserve HDR process data separately
when needed for analysis. If VRAM-only is retained, the UI must communicate its true range capacity
and partial completion. Neither choice changes the final-render precision contract.

**Gate:** a range larger than VRAM but smaller than the configured host allowance has predictable
behavior under the selected policy; promotion never blocks UI or rereads verified media; eviction
does not invalidate live presentation pins; memory/coverage indicators reconcile with live entries.
**Exclude:** lossily compressing canonical process results without a separate reviewed decision.

### G10 — Qualify sustained use, not just isolated frames

Run the matrix below after relevant changes; run the full matrix before claiming the redesign
complete. Exercise Linux native first, then Windows/macOS native where supported and the explicit
CPU fallbacks everywhere. Do not infer portability from Vulkan API availability. Preserve strict
`--require-device` and distinguish absence from failure. Gate native validity separately from pixel
parity and product performance.

## Workload matrix and proposed acceptance targets

Targets below are **proposed gates**, not measurements of today's build. G0 records a named
reference configuration, build mode, device/driver, output extent, refresh rate, and input storage.
If a target is infeasible, explain the measured bottleneck and review the target; do not quietly
replace the workload, precision, or test timing.

| Scenario | Evidence required |
| --- | --- |
| Simple 1080p solid/text animation; play/loop, drag/rotate/scale text and solid, undo/cancel | First acceptance scene: presented identity/intent, surface/cover transitions, pointer-to-feedback latency, no blank/flickering substitutions, memory plateau |
| A/B imported at source resolution; matching and 1080p compositions | Source read/hash/decode/upload counts; cold fill duration; byte peaks; actual output identities |
| Second pass, forward/backward seek, loop for 60 s | Achieved playback rate/drift and visible frame order; display-cache hits perform zero source I/O/decode/scene work |
| Transform/opacity edit on warmed media | Reuse unaffected source/subgraphs; zero source reads/hashes/decodes; measure input-to-visible-result p50/p95/p99 |
| Full/Half/Quarter, non-square pixels, ROI, nested blend/color/text | Correct descriptor and CPU parity; clearly identify which upstream costs resolution does and does not reduce |
| Exposure/gamma/channel, pan/zoom, two viewers | Correct per-viewer intent; no accidental CPU scene evaluation or source work for presentation-only changes |
| Fill while scrubbing; fill while exporting | Visible work remains responsive; finite preparation/submit depth; truthful partial coverage and provider |
| Small caps, device pressure, 10 fill/purge cycles, project swap/resize | All ownership categories accounted; stable settled allocation plateau; no stale pixels or unsafe retirement |
| Missing/replaced file, disconnected share, cancellation/device loss | Bounded cancellation; explicit diagnostics/fallback; no false cache hit, current-frame claim, or GPU provenance |
| Still/range/video/headless exports | CPU parity and explicit GPU provenance through actual production routes; one final boundary readback |

Provisional experience gates on the declared reference configuration:

- A simple solid/text scene must satisfy the playback and UI-delay gates before heavy-media
  qualification. During drag/rotate/scale, no unintended blank/clear frame or oscillation between
  old CPU and new resident frames is allowed. Geometry/handles track the visible frame; release,
  cancel, undo, and redo converge to the correct committed image. Record pointer-input-to-feedback
  and pointer-input-to-present-ack p50/p95/p99; provisional p95 ≤ 50 ms at 1080p. Preserve a correctly
  labelled last good image while newer work is pending.

- Fully retained 24/25/30 fps ranges play within 1% of intended elapsed duration over 60 seconds.
  Report missed/repeated presentations and clock drift separately. Also exercise 60 fps; record
  an explicit supported limit if the configured presentation platform cannot sustain it.
- Cached seek input-to-present-ack p95 ≤ 50 ms at 1080p and ≤ 100 ms for B at Full. Count UI delivery
  and presentation; do not time only cache lookup. Actual scanout is a separate measured value.
- During render/fill/export, a 10 ms monotonic UI probe records scheduling lateness relative to its
  ideal deadlines, including missed periods. Over each 60 s run, lateness p95 ≤ 16 ms and p99 ≤ 50 ms;
  report probe period, sample count, maximum lateness, and missed periods. This measures event-loop
  delay, not the raw interval between callbacks. No full-frame copy, hash, decode, fence wait, or
  allocation teardown belongs on the UI thread.
- After warmup, ten equivalent fill/purge cycles produce no unexplained monotonic growth. Treat
  growth above the larger of 5% or 128 MiB of the settled baseline as a failed investigation gate;
  report allocator block/driver residuals explicitly rather than calling every RSS byte a leak.
- At every admission point, actual charged plus reserved bytes stay within the assigned domain
  budgets. Quarantine cannot disappear from accounting or be evicted before retirement.
- Zero relevant Vulkan validation errors, zero wrong-frame/currentness claims in injected failure
  cases, unchanged CPU/GPU parity, and no new coverage exception.

## Supervisor acceptance and handoff

For each iteration, hand back the problem/trigger, exact changed boundaries, before/after trace,
focused regression result, native-run/skip status, CPU parity status, memory peak/retirement result,
and a rebuilt app at the tested revision. Include what was not exercised. Default to one coherent
package per PR with Conventional Commit messages; no change in this proposal itself creates a PR
or authorizes monitoring/merging unrelated work.

Recommended first delivery sequence: G0 plus G1, then G2, then G3 and G4. G5 can proceed alongside
those after its source-revision contract is written. Begin G6 accounting before G8 concurrency.
Only pursue G9 once working-set traces show which residency policy benefits the real workloads.
Keep ordinary node expansion secondary until the initial reliability and playback gates hold.

The completion criterion is an artist-visible, correctly identified frame delivered at a measured
latency with a bounded working set. More GPU dispatches, more tests, a green coverage list, or a
larger cache are insufficient substitutes.
