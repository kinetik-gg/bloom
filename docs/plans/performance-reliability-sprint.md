# Performance and reliability sprint

Status: active

Updated: 2026-10-22

Parent: [#306](https://github.com/kinetik-gg/bloom/issues/306)

## Baseline

The source audit inspected `f0dd8406`. Implementation starts from current main
`a7d693d8`, which includes #305. Revalidate each finding before editing.
Linux immutable source revisions, additional native ownership, bounded GPU batching and
short sustained playback evidence already exist. Preserve and extend them; do not redo them.
The owning architecture documents remain authoritative.

## Ordered packages

| Order | Issue | Gate | Package | State |
| --- | --- | --- | --- | --- |
| 00 | [#307](https://github.com/kinetik-gg/bloom/issues/307) | M0 | Revalidate current baseline and establish serialized local validation | Passed |
| 01 | [#308](https://github.com/kinetik-gg/bloom/issues/308) | M1 | Bound node utility memory and repair numeric/readout contracts | Implementing |
| 02 | [#309](https://github.com/kinetik-gg/bloom/issues/309) | M1 | Give recovery artifacts durable ownership across launches | Implementing |
| 03 | [#310](https://github.com/kinetik-gg/bloom/issues/310) | M1 | Move project Open/recovery I/O and rejected export retirement off UI | Implementing |
| 04 | [#311](https://github.com/kinetik-gg/bloom/issues/311) | M2 | Make warm RAM preview indexed and coverage incremental | Queued |
| 05 | [#312](https://github.com/kinetik-gg/bloom/issues/312) | M2 | Repair prepared-cache replacement and asynchronous CPU retirement | Queued |
| 06 | [#313](https://github.com/kinetik-gg/bloom/issues/313) | M2 | Separate raw source storage from request descriptors and deduplicate charges | Queued |
| 07 | [#314](https://github.com/kinetik-gg/bloom/issues/314) | M2 | Remove playback-driven Assets rebuilds and redundant UI value work | Queued |
| 08 | [#315](https://github.com/kinetik-gg/bloom/issues/315) | M2 | Make audio preparation and transport lifecycle asynchronous | Queued |
| 09 | [#316](https://github.com/kinetik-gg/bloom/issues/316) | M3 | Reuse video decoder/index state within bounded shared scheduling | Queued |
| 10 | [#317](https://github.com/kinetik-gg/bloom/issues/317) | M3 | Bound document edit cost and undo memory with structural sharing | Queued |
| 11 | [#318](https://github.com/kinetik-gg/bloom/issues/318) | M3 | Run integrated local performance, memory and route qualification | Queued |
| 12 | [#319](https://github.com/kinetik-gg/bloom/issues/319) | M3 | Independently audit the integrated sprint and close bounded follow-ups | Queued |

Acceptance and issue closure follow this order. Disjoint implementation may overlap within a
milestone only after the coordinator assigns file ownership. Implementation-ready does not mean
qualified. New defects are reported to the coordinator instead of expanding a package.

## Milestone validation policy

- M0: reconcile current source, establish the local runner, test its lock/evidence/failure behavior,
  and verify dependency/toolchain readiness. No product rebuild.
- M1: integrate packages 01–03; build the union of owning targets once; run node, recovery,
  project-session and export-lifecycle tests plus a focused numeric/resource sanitizer gate.
- M2: integrate packages 04–08; build the optimized application and affected targets once;
  run cache ownership/parity, required native production routes, CPU fallback and affected strict
  checks. Reuse M1 outputs and dependency/compiler caches.
- M3: integrate packages 09–10; run the final optimized build, sustained qualification in package
  11 and independent review in package 12. Audit repairs rerun their affected gates only.

Broad builds, full test suites, sanitizer sweeps and strict translation-unit analysis belong to
these coordinator-owned milestones. Each package still supplies regression source tests and cheap
changed-file formatting/diff checks. An early focused compile is permitted when it resolves a
specific integration uncertainty; it does not authorize a broad rebuild.

All validation runs directly on local runners. Do not dispatch GitHub Actions. Serialize expensive
jobs under one runner lock, bound parallel jobs, and retain exact source identity, commands, exits
and timings. An unavailable native device is Skipped/NotRun; required-device checks fail.
Use an isolated display for GUI validation. Never change the user's running application.

The [local validation runner](../development/local-validation.md) implements the shared lock,
named target/test unions, explicit broad gates and source-bound JSON evidence. M0 passed eight
toolchain-free runner tests, including source identity, contention, display isolation and skipped-test
classification, command failure, timeout and native-unavailable rejection. Synthetic milestone execution and lock-refusal behavior were also checked. No Bloom
product build was run for M0. The existing qualified dependency prefix and compiler tools are present;
their product compatibility is checked by the first milestone configure.

## Ownership and completion

Each implementation package defines allowed paths, contracts, non-goals, acceptance cases and
dependencies in its linked issue. The coordinator reviews changes before milestone admission.
Do not commit unrelated edits, generated build products, private paths or source media.

Preserve command atomicity, document/session/runtime boundaries, exact frame identity,
CPU reference semantics, real GPU preparation and all final-render routes. No new GPU coverage
exceptions or early retirement credit are permitted.

Close a package only after its regression evidence and assigned milestone pass on the recorded
source. Record unsupported platforms honestly. Broader platform qualification in #304 remains
separate from a Linux local pass.

## Performance acceptance

- A fully retained range performs no source reads/hashes, decoding, evaluation or new pixel-buffer
  allocations on warm replay. Cached-range restart submits no render work.
- Cache lookup, insertion and coverage updates avoid quadratic range-size growth; bounded
  continuations leave UI cancellation and input runnable.
- Saturated source-cache replacement eventually retains the current result after actual retirement,
  without another decode or over-budget admission.
- Count shared payloads once; distinguish configured ceilings, effective allowances, retained
  references, in-flight allocations and pending retirement.
- Measure visible frame identity, source-frame cadence, UI latency percentiles and cold-fill stages
  on declared representative workloads. Report measurements rather than unverified speedups.
- Ten fill/purge cycles and 60-second retained playback settle to an explained memory plateau.
  Exercise pressure, edits, export contention, cancellation and stale/replaced sources.

## Independent final review

Review node resource limits, numeric boundaries, UI readout parity, recovery ownership,
async shutdown, cache invalidation, admission and release ordering after implementation.
Assign actionable defects bounded follow-ups; do not widen into unrelated features.
