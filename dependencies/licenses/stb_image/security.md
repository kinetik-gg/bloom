# stb_image security review

Reviewed: 2026-09-15

## Disposition

Accepted risk for the v0 PNG/JPEG image-import exception in ADR 0020. Unlike stb_truetype,
this parser **does consume user-controlled files in process**. Bounds reduce resource exposure;
they do not provide memory-corruption isolation or a security guarantee. No claim of absence
of vulnerabilities is made. General codecs and audio are outside this exception.

The adapter permits only PNG/JPEG. kMaxImageDimension is 16384, kMaxImagePixels is 16777216,
and kMaxImageFileBytes is 67108864. It checks file size before allocation and dimensions before
decode. A per-decode allocator caps total live parser allocations at 268435456 bytes. The
resulting process image has a separate 268435456-byte budget. Directory scanning admits at most
100000 entries and a sequence span of 100000 frames. Cancellation is checked around parser
calls and during conversion/scanning; an individual bounded parser call cannot be interrupted.

Tests must exercise generated PNG8/PNG16 and JPEG plus truncated, malformed and huge-dimension
inputs, allocation failure, cancellation and sequence gaps. These fuzz-style hostile fixtures
are regression evidence, not a substitute for continuous fuzzing or process isolation.
Re-review on an upstream change, new advisory, limits change, format expansion or broader input
reachability. Out-of-process decoding and general video providers remain deferred.
