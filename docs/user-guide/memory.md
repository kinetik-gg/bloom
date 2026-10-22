# Memory

Bloom keeps intermediate graph results in its **operation cache** and finished display frames in
its **RAM preview**. The status bar reports each as "held / budget". Decoded video, audio,
thumbnail images and queued disk-cache writes have separate retention targets. Active exports
have separate reservation accounting.

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
transfers, live display pins and retirement can hold additional memory. Actual OS process/container
limits can constrain combined reusable cache retention; complete aggregate reservation accounting remains
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

## Choosing the budgets

Set operation-cache and RAM-preview budgets under **Edit → Preferences… → Memory & Caches**.
**Apply takes effect immediately.** Each nonzero value is that cache's selected target. Changing
one does not shrink the other. Auxiliary caches do not subtract from those targets.

**Automatic** (stored as 0) chooses stable defaults from physical memory when Bloom receives its
first usable worker sample. It does not keep reducing the target as RAM fills:

| Physical RAM | Operation target | RAM-preview target | Total |
| --- | --- | --- | --- |
| 8 GiB | 2 GiB | 2 GiB | 4 GiB |
| 16 GiB | 4.8 GiB | 3.2 GiB | 8 GiB |
| 32 GiB | 9.6 GiB | 6.4 GiB | 16 GiB |
| 60 GiB | 18 GiB | 12 GiB | 30 GiB |

The initial fallback is 1 GiB for operations and 2 GiB for preview when hardware capacity is
unknown. Smaller reported machines bound that fallback. Preferences shows resolved targets;
status shows bytes held against actual cache budgets. Targets are limits on retained results,
not preallocations or promises that every allocation will succeed.

The settings keys are decimal byte counts:

| Key | Meaning |
| --- | --- |
| `playback/operation-cache-bytes` | Selected operation-cache target |
| `playback/ram-preview-memory-bytes` | Selected RAM-preview target |

For example, 10737418240 and 6442450944 select 10 GiB and 6 GiB independently. Missing, zero or
unreadable settings select Automatic. Explicit settings are not reduced to a fraction of physical
or available RAM. Disk-cache settings still take effect after restart.

## Actual limits and memory availability

Bloom samples host/process memory on a worker every five seconds. Reusable retention begins at
zero until the first complete sample establishes process-limit readiness. Status and Preferences
read the cached result without probing the OS on the interface thread.

An actual OS process/container limit can reduce combined reusable targets. On Linux cgroup v2,
Bloom uses the smallest observed absolute process/ancestor limit; if all reusable cache targets
exceed it, those caches share it. The UI explains that constraint and reports the actual allowances.
It does not subtract an invented reserve or reduce cache targets whenever free memory falls.
Fresh complete observations permit immediate recovery. Once a finite limit is observed, stale or
failed readings cannot silently erase it; removal must be verified in the same hierarchy view.
Linux memory-v1 and Windows/macOS currently disclose host-only best effort for process limits.

Active rendering has separate admission based on measured available memory, actual process
headroom, device capability and already-held work. A small retention cache does not by itself mean
a frame is too large to render. Requests can wait or fail with a diagnostic when actual capacity
cannot cover their allocations; live pixels and active reservations remain valid. Separate GPU
owner partitions and final-export limits are still being audited.
For rooted RAM-preview and final pixel storage, Bloom reconciles the exact completed backing that
the OS has already counted in its free-space reading. This prevents a retained frame from reducing
available memory and being subtracted again as an owned allocation. A cache's held-byte display is
not used as memory credit, and unfinished allocation promises remain charged.

There is no automatic 25%/10% cache-trimming ladder or slow percentage recovery. Increasing swap
usage is reported as activity, without secretly changing your targets. The cache tooltip retains
its budget explanation during RAM-preview progress. These counters are not total application or
GPU-driver memory and do not guarantee protection from every allocation failure.

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
**Purging caches…** and disable themselves while a purge is running. Bloom stops preview, RAM
preview, background fill, asset thumbnail decode, and new export evaluation stages for the whole
purge. It waits for tasks already using the purgeable caches to finish before clearing; unrelated
Save and I/O work can continue. A result from an older cache-producing generation cannot land in
the cleared cache afterward. The GPU resident scene cache is cleared on its own owner thread;
outstanding GPU leases (the frame on screen, an export, an in-flight task) stay valid because only
the cache's own references are dropped. If a long-running task (for example an export) has not
retired within a short bounded wait, the purge stops safely without clearing and says so, rather
than leaving the commands disabled indefinitely; retry once the cache work finishes. Both commands
stay present even when the relevant cache is unavailable (the disk cache is disabled, or no preview
pipeline exists); the command then reports that clearly instead of silently doing nothing. Purging
the preview cache leaves decoded media and verified source revisions available. Purging media also
drops completed preview frames so they cannot hide a changed or unavailable source after refresh.

RAM Preview counts frames that are currently retained, including frames already cached before the
run. Its tooltip separates frames prepared this run from retained frames and the longest cached
span. If the range outgrows a real memory limit, caching stops with a partial result and does not
autoplay; use Play for ordinary manual playback. Purging or later eviction removes live coverage
without erasing the history of work already prepared. Half resolution reduces the display image
extent and retained image bytes; actual device capacity and finite resource limits still apply.
