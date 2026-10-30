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
limits constrain combined reusable cache retention. The headroom is the protection against running
the system out of memory; an allocation the driver or OS still refuses is a typed failure.

Cache-held bytes can fall after eviction or purge while the viewer or another consumer still holds
the same storage. A bounded rooted packed-host ownership slice tracks those pixel payloads across
their consumers; it does not change the cache's held/budget reading or measure total application memory.
The ownership accounting counts explicitly rooted pixel payloads. Its retirement accounting also
admits the backing's queue header and keeps both charged after the final consumer hands it to a
worker. That worker frees published backing before releasing its charge; cache-held cost may
already be zero while an external pin or queued release still owns memory. The bounded follow-on
passes its affected ownership and shutdown checks. Legacy storage, other metadata,
allocator overhead and native GPU
allocations remain separate; these checks do not measure total memory or guarantee release latency.

## Headroom: the only memory setting

Bloom uses the machine. Under **Edit → Preferences… → Memory & Caches** you choose two headroom
values, the memory Bloom leaves free for the rest of the system:

| Setting | Key | Default | Range |
| --- | --- | --- | --- |
| RAM headroom | `memory/ramHeadroomBytes` | 10% of RAM | 0 to 256 GiB |
| VRAM headroom | `memory/vramHeadroomBytes` | 10% of VRAM | 0 to 256 GiB |

Values are decimal byte counts. The RAM default is 10% of physical memory, or of the process or
container memory limit when that is smaller (about 6 GiB on a 64 GB workstation, 2.4 GiB under a
24 GB limit). The VRAM default is 10% of the largest graphics card's memory (about 2.4 GiB on a
24 GB card; 2 GiB until a card has been measured). A value equal to the default is not stored, so
the default follows the measured capacity; missing, malformed or out-of-range values select it.
Apply takes effect immediately.

**Usable RAM** is the available system memory Bloom measures (within any real OS or container limit)
minus the RAM headroom. **Usable VRAM** is the graphics driver's live memory budget minus the VRAM
headroom. The Viewer, RAM preview, playback and exports run as many frames at once as fit usable
memory. The reusable caches (operation results, RAM preview, decoded video and media, proxies and
audio) share what is left: together they grow by at most the free memory beyond the headroom, and
when other programs need memory and free memory falls below the headroom, every cache shrinks by
its share, so caches never push the computer into swap. Active work evicts them when it needs the
room.

Bloom watches memory continuously, not only when it starts work: a monitor samples the machine four
times a second (and on Linux wakes at once on a kernel memory-pressure (PSI) notification). As soon
as free memory drops below the headroom, every cache shrinks right away until free memory is back
above the headroom plus a margin (a quarter of the headroom, at least 512 MiB); between the two the
caches hold and do not refill what they just released. Bloom's own memory that the system has
moved to swap (or compressed, on zram and macOS) counts as used, not free, so caches are evicted
instead of being kept in swap. Free memory alone can look fine while the system is pushing other
programs into swap, so Bloom also treats the system's memory distress as pressure, whichever program
causes it: on Linux, swap traffic of 16 MiB/s or more, or programs stalled on memory (PSI) for a
tenth of the time ("some") or a fiftieth ("full"); on macOS, the warning or critical memory pressure
level; on Windows, the low-memory notification. Under such pressure the caches stop growing and give
back one margin four times a second until it stops. Evicted memory is returned to the operating system immediately, so Bloom's
resident size really drops. A larger RAM headroom applied in Preferences evicts at once. Inside a container or memory-limited session, clean file cache counts as available, as the
kernel reclaims it on demand. When work needs more than is free at that moment, it waits and
resumes as soon as memory is released; it is refused only when one frame cannot fit within the
computer's (or the container's) memory minus the headroom, and the refusal names the measured
numbers.

The earlier per-cache targets `playback/operation-cache-bytes` and
`playback/ram-preview-memory-bytes` are retired. Bloom ignores saved values for them and does not
migrate them: they were retention sizes, not safety margins, and the caches now size themselves
from usable memory.

Decoded video frames have their own retention target, read at startup from
`playback/decoded-video-cache-bytes` (decimal bytes). Missing or zero selects Automatic: one
sixteenth of physical memory, at least 256 MiB and at most 4 GiB (for example 1 GiB on a 16 GiB
machine). Like the other reusable caches it shares an observed OS process limit. Missing, zero or
unreadable settings select Automatic. Explicit settings are not reduced to a fraction of physical
or available RAM. Disk-cache settings still take effect after restart.

## Actual limits and memory availability

Bloom samples host/process memory on a worker every five seconds, and the machine's available
memory, its own swapped memory and memory stalls four times a second on a dedicated monitor thread
(see the headroom section). Reusable retention begins at
zero until the first complete sample establishes process-limit readiness. Status and Preferences
read the cached result without probing the OS on the interface thread.

An actual OS process/container limit bounds combined reusable retention. On Linux cgroup v2,
Bloom uses the smallest observed absolute process/ancestor limit; the caches share it. The UI explains that constraint and reports the actual allowances.
It does not subtract an invented reserve or reduce cache targets whenever free memory falls.
Fresh complete observations permit immediate recovery. Once a finite limit is observed, stale or
failed readings cannot silently erase it; removal must be verified in the same hierarchy view.
Linux memory-v1 and Windows/macOS currently disclose host-only best effort for process limits.

Active rendering is admitted against usable memory (measured capacity minus headroom) and the
work already admitted. Each frame claims what it needs, not a share of the machine: a 2048×858
float frame is about 28 MB. A frame that does not fit right now waits for other work to release
memory and then runs; Bloom refuses it only when it cannot fit usable memory on its own, and
writes the reason with the measured capacity, the claims held and the cached retention. Live pixels
and active reservations remain valid.
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

## When Bloom renders ahead

Bloom renders frames you have not looked at only when you ask for playback:

- **Play** renders and caches the frames just ahead of the playhead while it plays, as many at once
  as usable memory allows, and stops when playback stops.
- **RAM Preview** (the Viewer button, **Composition → RAM Preview**, Ctrl+Shift+Space or Numpad 0)
  fills the work area (the whole composition when there is no work area) and plays it once the
  range is cached. Pressing it again, or Esc, cancels.
- An idle composition is never filled in the background. An edit invalidates the cached frames it
  affects but does not start re-rendering them; only the frame on screen is rendered again.

RAM Preview counts frames that are currently retained, including frames already cached before the
run. Its tooltip separates frames prepared this run from retained frames and the longest cached
span. If the range outgrows a real memory limit, caching stops with a partial result and does not
autoplay; use Play for ordinary manual playback. Purging or later eviction removes live coverage
without erasing the history of work already prepared. Half resolution reduces the display image
extent and retained image bytes; actual device capacity and finite resource limits still apply.
