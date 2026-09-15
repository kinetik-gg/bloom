# minimp3 (2026-07-27 snapshot) Security Review

Reviewed: 2026-09-15

## Disposition

The v0 audio exception to ADR 0020 permits this compact decoder to parse user MP3 files in process.
This is an accepted risk, not an isolation claim. The file-size cap is 64 MiB and the decoded
sample budget is 48,000,000 interleaved samples. Bloom reads no file and calls no decoder until the
cap is checked; it checks the reported sample count before allocating decoded storage and validates
the rate, channels, read count, and final frame count afterward.

Malformed, truncated, and wrong-magic fixtures are required to fail with a typed error and never
publish a partial `AudioBuffer`. Decode is not interruptible once the third-party call begins;
cancellation and task shutdown remain future task-adapter responsibilities and must be checked
before and after the bounded call.

## Reachability And Re-review

Only the audio engine's explicit path-based probe/decode adapter reaches these headers. No decoder
types cross the Bloom-owned header boundary, and no document or UI path is added here. This review
must be renewed if the file cap, sample budget, supported containers, or in-process boundary
changes, or if an upstream security advisory affects the pinned commit.
