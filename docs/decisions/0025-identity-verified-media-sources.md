# ADR 0025: Identity-Verified Media Sources

Status: accepted

Date: 2026-10-23

## Context

Bloom tied every media source to a SHA-256 content digest and treated only a fresh full hash as
proof that the original still held those bytes. `Revalidate`, the default and final-render source
policy, therefore re-read, re-hashed and (for Linux video) re-copied the whole source on every
asset refresh, every Assets freshness tick during playback, every exported frame and every 65,536
sample audio block. A long movie made ordinary editing, playback and export spend most of their
time reading the same file again (issue #356).

## Decision

- A full hash records the file identity observed before and after the pass: length, modification
  time, status-change time, and volume plus file id (POSIX `stat` device/inode on Linux and macOS;
  the handle's volume serial and 128-bit file id, `LastWriteTime` and `ChangeTime` on Windows). A
  pass whose bracketing observations differ records nothing.
- `platform::VerifiedSourceDigests` is a bounded process-wide memo of (absolute path, identity) to
  digest. While a later identity observation is identical, the recorded digest stands for the
  bytes. Any difference — touch, rewrite, rename-over, another volume, a missing file — forces a
  full rehash, so changed content still reports `SourceChanged` and the relink diagnostic.
- `Revalidate` now means "observe the original's identity, rehash only when it changed". It
  remains the default, and final preparation still forces it and still rejects missing or changed
  media before memoized pixels are accepted. `Retained` keeps its meaning: use the last verified
  revision without touching the original.
- Verified encoded revisions stay the decode authority. Linux video still decodes only an
  anonymous immutable snapshot; a live snapshot is shared by digest between sessions through a
  non-owning index, so it is never copied twice while any session retains it. Still-image
  revisions in the evaluator cache are reused under `Revalidate` after the same identity check.
- Every whole-file source read for hashing or snapshot capture increments process-wide work
  counters (`platform::sourceReadWork()`), which tests use to prove zero full-file reads during
  playback, ordinary edits and per-frame export once a source is verified.

## Consequences

- One full read per source per process at import or first use; afterwards one `stat` (or one
  handle query on Windows) per verification.
- The memo is runtime state and is never saved; reopening a project in a new process hashes each
  source once again.
- Residual risk, accepted: content changed in place while length, modification time and
  status-change time all stay identical is not detected until the identity changes. On POSIX
  status-change time cannot be set by ordinary user calls; on Windows `ChangeTime` can be set
  explicitly, and coarse-timestamp or network filesystems may hide a same-size rewrite within one
  timestamp tick. Explicit refresh/relink and the immutable decode snapshot bound the damage: a
  decode never mixes old and new bytes.
- The behavior is identical on Linux, macOS and Windows; only the identity query differs.

## Related

- [`../architecture/media-io.md`](../architecture/media-io.md)
- [`../architecture/frame-output.md`](../architecture/frame-output.md)
