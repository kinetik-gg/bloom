# Memory

Bloom keeps two caches in memory so that work you have already seen does not have to be computed
again: the **operation cache**, which holds intermediate results of the node graph, and the **RAM
preview**, which holds finished display frames so playing a range a second time is instant. Both
are reported in the status bar's cache cell, each as "held / budget".

## How much memory Bloom gives itself

Bloom chooses its own budgets from the size of the machine, deliberately conservatively, because a
motion-graphics application is never the only thing running.

It first sets aside a **reserve** for everything that is not Bloom -- the operating system, the
window compositor, your browser, your decoders. The reserve is 40% of physical memory, and never
less than 8 GiB. What remains is the most Bloom will use even if you ask it to.

Without being asked, Bloom takes less than that: its two caches together never default to more than
half of physical memory, nor more than 80% of the memory the system reported as free when Bloom
started. Whichever of those is smallest wins, and the total is then split 60% to the operation cache
and 40% to the RAM preview.

| Your machine | Operation cache | RAM preview | Total |
| --- | --- | --- | --- |
| 8 GB | 1 GB | 2 GB | 3 GB |
| 16 GB | 4.8 GB | 3.2 GB | 8 GB |
| 32 GB | 9.6 GB | 6.4 GB | 16 GB |
| 64 GB | 19.2 GB | 12.8 GB | 32 GB |

Small machines are held at a 3 GB floor -- 1 GB of operation cache and 2 GB of RAM preview -- so
the preview still works on a laptop.

## Changing the budgets

Two settings keys override the defaults. Both are **byte counts written as decimal numbers**, and
both are read once, when Bloom starts.

| Key | What it sets |
| --- | --- |
| `playback/operation-cache-bytes` | The operation cache's budget |
| `playback/ram-preview-memory-bytes` | The RAM preview's budget |

For example, `playback/ram-preview-memory-bytes = 6442450944` asks for a 6 GiB RAM preview.

An override is honoured as written, up to the reserve: you may give Bloom more than it would have
chosen for itself, but not so much that the machine has nothing left. Setting only one key leaves
the other to be computed from what remains. Setting both to more than the machine can give reduces
them in proportion, so neither setting is silently ignored. A missing, zero, or unreadable value
simply means "use the default". The status bar always shows the budgets Bloom is actually using,
which is what to check after an edit.

## When memory runs short

If the system reports less free memory than Bloom reserved for it, Bloom trims both caches to half
their budgets and shows **Memory pressure: caches trimmed** in the status bar. Nothing is lost:
trimmed entries are results that can be computed again, the budgets themselves are unchanged, and
the caches refill as you keep working once the machine recovers. If you see this often, either
something else on the machine is using a great deal of memory, or your overrides are larger than
this machine can support.

Media Bloom has decoded from disk is cached on disk as well, under a separate budget; see the media
cache settings for that. Clearing the disk cache does not affect either memory budget.
