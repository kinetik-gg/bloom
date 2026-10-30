# ADR 0027: Preview hardware video decode and decode capacity

Status: accepted

Date: 2026-10-23

## Context

Issue #366 audited the Linux FFmpeg read path. FFmpeg was built without x86 SIMD and decoded on
one thread; the VA-API decode code was never selected and hard-coded one render node; every frame
step other than +1 re-seeked; a session refused concurrent callers with `Busy`; only four sessions
were retained; the decoded cache was a fixed 256 MiB; and each decoded frame was hashed seven
times between the worker and the color conversion.

## Decision

- FFmpeg's x86 SIMD is enabled through a source-built, build-only NASM (see
  [`dependency-intake.md`](../architecture/dependency-intake.md)).
- Software decode is threaded within a memory bound derived from the media-v1 worker address
  space; intra-only codecs use slice threads. It remains the reference path and keeps
  `DecodedSemanticExact`.
- A retained decoder continues forward inside the GOP and seeks by the decode time of the key
  packet presented at or before the target.
- VA-API decode is a separate preview-only worker generation with its own execution key. It
  discovers render nodes at run time, proves its first frame byte-identical to the software
  reference in every worker generation, and falls back explicitly and permanently per source on
  any refusal. Final rendering never uses it. Decoded-cache keys and GPU raw-upload identities
  include the route.
- Sessions queue callers; retained sessions are admitted by estimated worker memory and the
  source-snapshot file quota; the decoded cache target is configurable and otherwise a share of
  physical memory registered with the process memory ledger.
- Plane digests are computed once in the worker and verified once at the host boundary.

## Consequences

- Hardware decode changes performance only. Its parity tolerance is zero; a mismatch is a typed
  fallback, not a degraded frame.
- macOS (VideoToolbox) and Windows keep their own providers; the session API is platform-neutral
  and reports software provenance where no hardware route exists.
- The details and the measured before/after latencies live in
  [`media-io.md`](../architecture/media-io.md#video-read-implementation-media-3).
