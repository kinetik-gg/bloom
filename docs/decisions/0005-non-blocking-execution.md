# ADR 0005: Non-Blocking Execution

Status: accepted

Date: 2026-08-25

## Context

Professional compositing routinely performs expensive decode, evaluation, rendering, caching,
project I/O, and GPU operations. Running this work in Qt callbacks would freeze interaction and
undermine the professional interaction quality Bloom requires. Ad hoc background
threads would still leave cancellation, stale results, overload, diagnostics, and shutdown
inconsistent.

## Decision

- Treat UI responsiveness as an architecture invariant.
- Run all work whose latency depends on project size, media, storage, drivers, external processes,
  or cache state outside the Qt UI thread.
- Route non-trivial work through an application task system with stable identity, bounded queues,
  semantic priority, cooperative cancellation, structured progress, structured diagnostics, and
  explicit terminal outcomes.
- Give evaluation and render tasks immutable document snapshots and explicit requests.
- Tag results with their source revision and request generation; application services suppress stale
  publication.
- Keep task primitives below the Qt boundary. UI code observes them through adapters and never
  blocks waiting for completion.
- Treat device loss and shutdown as staged task-lifecycle events. Preserve project truth, cancel or
  fail affected work clearly, invalidate derived resources, and never publish partial output as
  success.

The detailed contract is defined in `docs/architecture/task-system.md`.

## Consequences

- Viewer scrubbing, panel interaction, cancellation, and progress remain responsive during heavy
  work.
- Runtime services require explicit lifetime, ownership, backpressure, and result-publication rules.
- Third-party calls that cannot be interrupted must be isolated and may have delayed cancellation,
  but cannot hold the UI thread.
- Tests need slow fakes, stale-result cases, device-loss simulation, and active-work shutdown cases.
- Some operations that appear simple, including project save and media probing, require asynchronous
  staging from the beginning.
