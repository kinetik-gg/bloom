# Memory

Bloom keeps intermediate graph results in its **operation cache** and finished display frames in
its **RAM preview**. The status bar reports each as "held / budget". Decoded video, audio,
thumbnail images, queued disk-cache writes and the export frame queue also share the memory
allowance.

## How much memory Bloom gives itself

Bloom adjusts its cache cap every five seconds as other applications use or release memory.
Its configured default leaves the larger of 8 GiB or 40% of physical RAM for the rest of the
machine, and never takes more than half of physical RAM. The operation/preview ceilings start
with this split:

| Physical RAM | Operation ceiling | RAM preview ceiling | Configured total |
| --- | --- | --- | --- |
| 8 GiB | 1 GiB | 2 GiB | 3 GiB |
| 16 GiB | 4.8 GiB | 3.2 GiB | 8 GiB |
| 32 GiB | 9.6 GiB | 6.4 GiB | 16 GiB |
| 60 GiB | 18 GiB | 12 GiB | 30 GiB |

The effective cap is the smallest of the configured total, half of physical RAM, and 80% of
**currently available memory plus the bytes Bloom's caches already hold**. Adding those held bytes
back means filling a cache does not by itself make Bloom shrink it again. The policy has a 3 GiB
floor, bounded by physical RAM on smaller machines; pressure can still reduce actual cache
budgets below that floor. Auxiliary caches share the total, so the operation and preview budgets
shown in the status bar can be smaller than the table's ceilings.

For example, on a 16 GiB machine with 3 GiB available and 4 GiB cached, the cap can fall from
8 GiB to 5.6 GiB. On a 60 GiB machine with 12 GiB available and 8 GiB cached, it can fall from
30 GiB to 16 GiB. Changes must exceed 10%, with at least five seconds between cap changes, to
avoid reacting to every small fluctuation.

Hover over the status bar's cache cell to see the **effective cap**, **configured total**,
**MemAvailable**, pressure state and the percentage currently allowed for cache admission.
"MemAvailable" uses the operating system's available-memory estimate; an unavailable reading is
shown explicitly.

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
total. Those numbers are ceilings, not guaranteed allocations. If available memory plus held
cache bytes falls to 10 GiB, the effective cap can fall to 8 GiB even with these overrides.

Setting one key leaves the other at its default where space permits. Oversized overrides are
reduced proportionally; they cannot bypass the machine's live cap. A missing, zero or unreadable
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

The export queue's usual 4 GiB concurrent allowance also falls with the shared cap; each job
still has a 2 GiB limit. Bloom keeps memory already reserved by active exports. New export
reservations, or requests for more memory by an existing export, are refused if they would exceed
the reduced allowance. Admission becomes possible again when active exports release enough
memory or recovery raises the allowance. Trimming never frees a live export product.

After the machine recovers, Bloom waits ten seconds, then restores budgets one step every ten
seconds: 10% to 25%, then 50%, then 100%. The effective cap itself grows by at most 25% per
step. New pressure interrupts recovery immediately, and a swap file that stays full but unchanged
no longer counts as pressure, so it does not keep the caches trimmed.

Decoded media stored on disk has a separate disk-space budget. Its pending writes consume RAM
and follow the memory limits above; clearing stored disk entries does not change your memory
settings.
