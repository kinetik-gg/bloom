# stb_image security review

Reviewed: 2026-09-15; capacity limits re-reviewed 2026-09-20

## Disposition

Accepted risk for the v0 PNG/JPEG image-import exception in ADR 0020. Unlike stb_truetype,
this parser **does consume user-controlled files in process**. Bounds reduce resource exposure;
they do not provide memory-corruption isolation or a security guarantee. No claim of absence
of vulnerabilities is made. General codecs and audio are outside this exception.

The adapter permits only PNG/JPEG. It reads through stb's callback stream and does not buffer the
encoded file, so there is no whole-file cap. Geometry is bounded by stb's own per-axis
`STBI_MAX_DIMENSIONS` (2^24) and its integer sample-count overflow checks, not by an absolute
per-axis or total-pixel ceiling. Decoded RGBA32F storage plus the RGBA16 staging buffer must fit the
caller's explicit `pixelBudget` before the parser is entered. A per-decode, thread-local allocator
caps total live parser allocations at that same request budget, scoped to one request so concurrent
decodes with different budgets cannot interfere. The resulting process image has its own
caller-supplied budget, defaulting to 268435456 bytes. Directory scanning admits at most
100000 entries and a sequence span of 100000 frames. Cancellation is checked around parser
calls and during conversion/scanning; an individual bounded parser call cannot be interrupted.

Tests must exercise generated PNG8/PNG16 and JPEG plus truncated, malformed and huge-dimension
inputs, allocation failure, cancellation and sequence gaps. These fuzz-style hostile fixtures
are regression evidence, not a substitute for continuous fuzzing or process isolation.
Re-review on an upstream change, new advisory, limits change, format expansion or broader input
reachability. Out-of-process decoding and general video providers remain deferred.
