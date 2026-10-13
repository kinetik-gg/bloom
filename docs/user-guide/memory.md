# Memory

Bloom keeps intermediate graph results in its **operation cache** and finished display frames in
its **RAM preview**. The status bar reports each as "held / budget". Decoded video, audio,
thumbnail images, queued disk-cache writes and the export frame queue also share the memory
allowance.

Ordinary GPU preview prepares the scene and display on the GPU, then transfers the final display
image once for widget painting. Its status says **GPU-prepared / CPU-painted**. RAM preview retains
that immutable host display storage, including metadata, without a second native copy of every
finished frame. A tight Full 1920×1080 display has 8,294,400 pixel bytes; Half has 2,073,600 before
metadata. The GPU native route remains separately supported. Channel display changes use a bounded
CPU worker and disclose CPU presentation; they do not change the scene's GPU provenance.

Finished packed preview retention is admitted by bytes, not a fixed frame count. The count depends
on the returned display extent, format/metadata and current per-pool allowance; the resolution
selector alone does not prove the actual returned extent. A range that outgrows that allowance is
partial, and later requests can evict older least-recently-used frames, shifting green coverage.

Cache counters describe held products, not total process or driver memory. Active work, temporary
transfers, live display pins and retirement can hold additional memory. Process/container-limit
headroom now constrains reusable cache retention; complete aggregate reservation accounting remains
under development. These limits do not guarantee total process memory or prevent every OOM.

Cache-held bytes can fall after eviction or purge while the viewer or another consumer still holds
the same storage. A bounded rooted packed-host ownership slice tracks those pixel payloads across
their consumers; it does not change the cache's held/budget reading or measure total application memory.
The ownership checkpoint counts explicitly rooted pixel payloads. Its retirement follow-on also
admits the backing's queue header and keeps both charged after the final consumer hands it to a
worker. That worker frees published backing before releasing its charge; cache-held cost may
already be zero while an external pin or queued release still owns memory. The bounded follow-on
passes its affected ownership and shutdown checks. Legacy storage, other metadata,
allocator overhead and native GPU
allocations remain separate; these checks do not measure total memory or guarantee release latency.

## How much memory Bloom gives itself

Bloom samples memory on a worker every five seconds as other applications use or release memory.
At startup reusable cache retention stays at zero until the first usable sample; ordinary frame
processing still works. Status bars observe one shared sampler and never read OS memory directly.
Its configured default leaves the larger of 8 GiB or 40% of physical RAM for the rest of the
machine, and never takes more than half of physical RAM. The operation/preview ceilings start
with this split:

| Physical RAM | Operation ceiling | RAM preview ceiling | Configured total |
| --- | --- | --- | --- |
| 8 GiB | 1 GiB | 2 GiB | 3 GiB |
| 16 GiB | 4.8 GiB | 3.2 GiB | 8 GiB |
| 32 GiB | 9.6 GiB | 6.4 GiB | 16 GiB |
| 60 GiB | 18 GiB | 12 GiB | 30 GiB |

The effective reusable allowance can be smaller than these configured ceilings. Host policy also
considers half of physical RAM and 80% of currently available memory, with a physical-bounded
3 GiB fallback floor. Current cache counters are not added back as reclaimable credit. Auxiliary
caches share the allowance, so operation and preview budgets can be smaller than the table.

Automatic budgets leave room for the operating system and rendering; a configured ceiling is not
an allocation guarantee. Saved override values remain intact while host capacity, per-pool shares
and verified process ceilings determine the effective allowance. An 8 GiB diagnostic launcher limit
belongs to a private test/launch harness, not the normal user cache default or launch policy.

On Linux cgroup v2, Bloom pairs each visible process/ancestor memory limit with its own usage and
reserves the smaller of 1 GiB and the larger of 16 MiB or one eighth of that limit. Remaining
headroom further clamps reusable retention. A hard limit, including zero headroom, takes precedence
over the host floor and immediately lowers retention even during cooldown. The first finite sample
starts at no more than half that headroom. Recovery waits ten seconds and increases conservatively;
it does not immediately reclaim all apparent free space.

Linux memory-v1 and Windows/macOS currently use explicit host-only best effort for process limits.
Unavailable or partial readings are disclosed. Once a finite process ceiling has been measured,
failed, stale or unsupported samples retain that constraint rather than expanding it. A verified
removal of previously observed limits permits bounded recovery only when all remembered domains
explicitly report no finite limit in the same verified hierarchy view. It does not prove hidden
limits absent. See [the owning policy](../architecture/evaluation-primitives.md) for that proof.
This is reusable-cache protection, not a guarantee for live processing, exports, GPU/driver
allocations or total process memory.

Live preview and export work additionally use conservative phase reservations before preparation
and execution. A refused phase can defer eligible preview work until capacity changes; it cannot
bypass admission through CPU rendering. These reservations are separate from cache counters and
do not prove unique physical memory accounting or guarantee that every workload fits.

Hover over the cache cell for **Effective reusable cache allowance**, **Configured cache ceiling**,
process-limit provenance, available memory and pressure/admission state. The configured ceiling is
resolved against host capacity; it may differ from the raw values saved in Preferences.

## Changing the budgets

Set these under **Edit → Preferences… → Memory & Caches**. They are read when Bloom starts, so a
change takes effect after restart. Underneath, two settings keys set the ceilings; both are **byte
counts written as decimal numbers**:

| Key | What it sets |
| --- | --- |
| `playback/operation-cache-bytes` | The operation cache's configured ceiling |
| `playback/ram-preview-memory-bytes` | The RAM preview's configured ceiling |

For example, `playback/operation-cache-bytes = 10737418240` and
`playback/ram-preview-memory-bytes = 6442450944` request 10 GiB and 6 GiB: a 16 GiB configured
total. Those numbers are ceilings, not guaranteed allocations. If available memory falls to 10 GiB,
the host allowance can fall to 8 GiB even with these overrides;
process headroom can clamp reusable retention further.

Setting one key leaves the other at its default where space permits. Oversized overrides are
resolved proportionally against host capacity; saved override values remain unchanged. They cannot
bypass the effective reusable clamp. A missing, zero or unreadable
value uses the default. An explicitly small ceiling is never enlarged to satisfy the policy
floor. The status bar shows the budgets actually in use.

## When memory runs short

Bloom enters pressure mode if available memory falls below its reserve, or if swap is **actively
growing** rather than merely occupied. Swap that was filled long ago and no longer changes does not
trigger trimming, however full it is; a single high reading is only a baseline, and a later rise is
measured against the previous reading. Once swap pressure is active a smaller rise keeps it active,
so a borderline value cannot make the caches flap. At the first poll Bloom trims caches to **25% of
their effective budgets**. If pressure remains at the next poll, it trims to **10%**. New cache
entries must fit those reduced limits too, so ongoing imports cannot immediately refill the caches.

The status bar shows **Memory pressure: caches trimmed** once per episode. Swap pressure also
shows **Swap pressure: caches trimmed** as a separate notice. Your project and configured
settings are unchanged; evicted results can be computed again. Active decoding, playback and disk
writes may still hold references until that work releases them, so process memory need not fall
by exactly the cache counter's change.

The export queue's usual 4 GiB concurrent allowance follows separate host/live-work admission,
including host-pressure reductions; cold reusable zero and the process-retention clamp do not
disable live export reservations. Each job
still has a 2 GiB limit. Bloom keeps memory already reserved by active exports. New export
reservations, or requests for more memory by an existing export, are refused if they would exceed
the reduced allowance. Admission becomes possible again when active exports release enough
memory or recovery raises the allowance. Trimming never frees a live export product.

After the machine recovers, Bloom waits ten seconds, then restores budgets one step every ten
seconds: 10% to 25%, then 50%, then 100%. The effective cap itself grows by at most 25% per
step, with process-headroom growth additionally limited to half the remaining gap. New pressure
interrupts recovery immediately, and a swap file that stays full but unchanged
no longer counts as pressure, so it does not keep the caches trimmed.

Decoded media stored on disk has a separate disk-space budget. Its pending writes consume RAM
and follow the memory limits above; clearing stored disk entries does not change your memory
settings.

## Purging caches

**Edit → Purge…** holds two explicit maintenance commands that free derived runtime data. Neither
one edits the project: no command, revision, dirty state, or undo step is produced, and no live
frame held by the viewer or an export is invalidated — the purge only drops the caches' own
references, so anything still in use stays valid until its owner releases it. Neither command asks
for confirmation; both clear only rebuildable derived data.

- **Purge preview cache** drops every retained RAM preview frame, including the cache's references
  to GPU-resident frames, the evaluator's cached derived operation results, and the shared GPU-scene
  coverage, prepared-upload, and resident GPU scene stores. Any preview or RAM preview frame still
  being prepared is retired first. The frame on screen stays valid; the next frame you ask for is
  computed again, so the preview cache's held-bytes reading drops to empty and refills only as you
  work.
- **Purge media cache** clears the on-disk decoded-frame store, the evaluator's in-memory decoded
  still-image and decoded-video media, retained source revisions and completed RAM preview frames,
  the prepared-upload store, and the resident GPU scene store
  (which retains decoded/uploaded media sources). Your source files are never read, written, or
  changed: the caches hold decoded or converted copies only, and the next use decodes again. Derived
  operation results are left alone — those belong to **Purge preview cache**. The next preview
  request verifies the source again before reusing any derived operation result.

The work runs off the interface thread on Bloom's existing task system, and the commands show
**Purging caches…** and disable themselves while a purge is running. Bloom stops the preview, RAM
preview, and background producers for the whole purge, then waits for work already in flight to
finish before it clears, so a result that was already being computed cannot land in a cache after
the purge. The GPU resident scene cache is cleared on its own owner thread; outstanding GPU leases
(the frame on screen, an export, an in-flight task) stay valid because only the cache's own
references are dropped. If a long-running task (for example an export) has not retired within a
short bounded wait, the purge stops safely without clearing and says so, rather than leaving the
commands disabled indefinitely; retry once the work finishes. Both commands stay present even when
the relevant cache is unavailable (the disk cache is disabled, or no preview pipeline exists); the
command then reports that clearly instead of silently doing nothing. Purging the preview cache
leaves decoded media and verified source revisions available. Purging media also drops completed
preview frames so they cannot hide a changed or unavailable source after refresh.

RAM Preview counts frames that are currently retained, including frames already cached before the
run. Its tooltip separates frames prepared this run from retained frames and the longest cached
span. If the range outgrows a real memory limit, caching stops with a partial result and does not
autoplay; use Play for ordinary manual playback. Purging or later eviction removes live coverage
without erasing the history of work already prepared. Half resolution reduces the display image
extent and retained image bytes; actual device capacity and finite resource limits still apply.
