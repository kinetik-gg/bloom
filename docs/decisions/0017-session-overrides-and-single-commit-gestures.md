# ADR 0017: Session Overrides And Single-Commit Gestures

Status: accepted

Date: 2026-08-25

## Context

Direct manipulation must feel immediate without turning every pointer move into project history,
document revisions, graph compiles, or add-on callbacks. Mutating the document continuously and
merging commands later obscures the actual transaction, makes cancellation unreliable, and can
leave animated values or graph-driven parameters in ambiguous states.

Current time, hover, focus, and an unfinished drag are editing-session concerns. They must affect a
preview request without becoming project truth.

## Decision

- Keep current time and active interactions in the composition session, outside the durable
  document and undo stack.
- During a drag, preserve the base document revision and parameter identity and publish a typed,
  request-scoped parameter override to snapshot compilation. Overrides are validated through the
  parameter schema, lowered as constants for that request only, and included in deep plan/cache
  identity.
- Do not mutate the document on pointer motion. On release, commit exactly one typed document
  transaction: set a constant, update the key at the exact current time, or insert one key. A
  zero-delta gesture commits nothing.
- Cancel the interaction on explicit cancel, secondary-button cancellation, capture loss, deleted
  or missing targets, incompatible source changes, a stale base revision, or a changed viewport
  mapping. Cancellation removes the override and creates no undo entry.
- Keep command boundaries meaningful. Bloom does not depend on command merging to repair gesture
  history and does not emit one command per pointer event.
- Treat scrubbing and direct manipulation as interactive preview requests with bounded trailing
  coalescing. Retain at most one active handle and one newest pending request per preview owner. A
  superseded active task is cancelled but remains the submission gate until terminal; an end-of-
  gesture event bypasses the trailing delay without bypassing that gate.

The detailed interaction and time contract is maintained in
[`../architecture/animation-and-time.md`](../architecture/animation-and-time.md).

## Consequences

- Undo matches artist intent: one completed gesture is one understandable step.
- Escape and capture loss restore the unmodified project exactly.
- The evaluator can show transient values through the canonical compile/evaluate path without
  creating a second render model.
- Request overrides are an application/session mechanism, not hidden durable parameter sources or
  a general scripting back door.
