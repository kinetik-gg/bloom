# Bloom Documentation

Status: canonical

Updated: 2026-08-25

This directory is Bloom's current source of truth. Historical notes outside this repository may
provide research or rationale, but they are not binding until adopted here.

## Reading Order

1. [`product/foundation.md`](product/foundation.md) — product identity, current decisions, v0 proof,
   and non-goals.
2. [`product/quality-bar.md`](product/quality-bar.md) — professional artist experience, reliability,
   responsiveness, and VFX expectations.
3. [`standards/strategy.md`](standards/strategy.md) — standards-first pipeline and interoperability
   policy.
4. [`ux/compositing-workspace.md`](ux/compositing-workspace.md) — the UI sketch as an interaction
   contract.
5. [`architecture/overview.md`](architecture/overview.md) — boundaries, state ownership, and
   dependency direction.
6. [`architecture/task-system.md`](architecture/task-system.md),
   [`architecture/gpu-backend.md`](architecture/gpu-backend.md), and
   [`architecture/platform-support.md`](architecture/platform-support.md) — execution and platform
   contracts.
7. [`roadmap.md`](roadmap.md) — proof-oriented implementation sequence.
8. [`decisions/`](decisions/) — accepted architecture decision records.

## Status Vocabulary

- `accepted`: a binding current decision.
- `working`: the current direction, expected to be tested and refined.
- `proposed`: an option under discussion.
- `superseded`: retained for history but no longer current.

Documents should separate accepted decisions from working assumptions and open questions. Avoid
presenting deferred product scope as an implementation commitment.

## Documentation Rules

- Update the owning document rather than duplicating a decision.
- Record architecture changes as decision records when they affect multiple subsystems.
- Keep implementation-specific detail close to the code it governs.
- Never include machine-specific paths, credentials, tokens, or private environment details.
