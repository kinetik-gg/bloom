# dr_wav 0.14.6 Security Review

Reviewed: 2026-09-15

## Disposition

The v0 audio exception to ADR 0020 permits this small decoder to parse user WAV files in process.
This is an accepted risk, not an isolation claim. The file-size cap is 64 MiB and the decoded
sample budget is 48,000,000 interleaved samples. Bloom checks both limits before entering the
decoder and validates every returned rate, channel count, frame count, and read count afterward.

Malformed and truncated WAV fixtures are required to fail with a typed error and never publish a
partial `AudioBuffer`. Decode is not interruptible once the third-party call begins; cancellation
and task shutdown are therefore outside this engine-only slice and must be checked before and
after the bounded call by a future task adapter.

## Reachability And Re-review

Only the audio engine's explicit path-based probe/decode adapter reaches the header. The decoder's
types do not cross the Bloom-owned header boundary, and no document or UI path is added here. This
review must be renewed if the file cap, sample budget, supported containers, or in-process boundary
changes, or if an upstream security advisory affects the pinned commit.
