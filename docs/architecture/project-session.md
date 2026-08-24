# Application Project Session And Persistence Lifecycle

Status: accepted

Implementation status: synchronous Qt-free ownership, editable/degraded/preserved-read-only state,
immutable document access, command forwarding, clean revision, dirty state, and savepoint acceptance
are implemented. Runtime session generations, asynchronous Open/Save, round-trip state, unsaved
continuations, and result acceptance remain pending.

Updated: 2026-08-25

## Purpose

`ProjectSession` is Bloom's application-owned boundary between durable project truth, command
history, Project I/O, and replaceable UI projections. It makes New, Open, Save, Save As, close, and
quit non-blocking without allowing a stale task to replace newer artist intent.

[ADR 0018](../decisions/0018-application-project-session-and-persistence-intent.md) fixes this
ownership and intent model.

The byte-level format, preservation rules, resource limits, and platform publication outcomes are
defined in [`project-format.md`](project-format.md). The scheduler remains transport; this document
owns application acceptance and dirty-state semantics.

## Ownership And Boundary

The first proof has one active `ProjectSession` owned by the application composition root. It is a
Qt-free application service, outlives editor panels, owns a runtime `ProjectSessionId`, and advances
`SessionResultAcceptanceGeneration` when installed content changes. Its decoded-document state
owns:

- one `Document` and its current immutable snapshot
- one `CommandStack` bound to that document
- Project I/O `RoundTripState`
- source container/document versions and open diagnostics
- `Editable` or `DegradedEditable` capability state
- an optional display path, canonical target identity, and external-file fingerprint
- the exact clean revision and the most recent publication warning

A preserved read-only open whose unknown core data cannot construct a trusted `Document` instead
owns a `PreservedReadOnlyProject`: bounded original archive data, source path and identity, format
versions, summary metadata, and diagnostics. It has no command stack and supports only inspection,
byte-preserving Save Copy, or closing.

`RoundTripState` belongs to Project I/O and the project session, not `Document`. Its public wrapper
is move-only. It may internally share immutable storage so a save request can capture a lifetime-safe
read view without copying an entire retained DOM on the authoring thread.

Composition, preview, editor, scripting, and future headless contexts observe the session and issue
commands through it. They never own the document, command stack, file path, round-trip state, or
project tasks. A panel replacement neither cancels nor joins project work.

## Identity And Generations

The following identities are deliberately separate:

| Identity | Purpose | Persisted |
| --- | --- | --- |
| durable `ProjectId` | references inside one project document | yes |
| `ProjectSessionId` | identifies one runtime project-session service lifetime | no |
| `SessionResultAcceptanceGeneration` | rejects result-driven mutation after installed session content is replaced | no |
| `OpenIntentGeneration` | orders competing Open requests for the current acceptance generation | no |
| `SessionPathIntentGeneration` | prevents an older Save As result from changing the active path | no |
| `PublicationIntentId` | orders application-wide file replacement for one canonical target | no |

The runtime `Project` task owner uses a fresh `TaskOwnerId` for each installed session. It never casts
or derives that value from durable `ProjectId`, because different files may reuse durable numeric
IDs.

`SessionResultAcceptanceGeneration`, `OpenIntentGeneration`, and
`SessionPathIntentGeneration` are separate unsigned 64-bit strong values in
`1..18446744073709551615`; zero is always invalid. A new `ProjectSession` initializes each value to
one. Every advance is checked before changing state, and an issued value is never wrapped or reused:

- if `SessionResultAcceptanceGeneration` cannot advance, New and Open admission are rejected before
  replacement or worker submission; Bloom keeps the current installed content and reports
  `RuntimeIdentityExhausted`;
- if `OpenIntentGeneration` cannot advance, a new Open is rejected before it cancels or supersedes
  the currently desired Open; and
- if `SessionPathIntentGeneration` cannot advance, a chosen Save As target is rejected before
  staging and its publication admission is abandoned. Plain Save to the existing path may continue
  to capture the final path generation.

Exhaustion is process-lifetime state. Restarting Bloom creates a new runtime session; no recovery
path resets a live counter or aliases an older generation.

Installing New or a successful Open advances `SessionResultAcceptanceGeneration` and cancels
replaceable work owned by the previous installed content. That invalidates only acceptance of task
results into `ProjectSession`; it does not revoke a `PublicationIntentId`, change per-target ordering,
or relabel a platform publication outcome. A late worker may finish, publish a previously admitted
save when its publication intent still wins, or populate a content-addressed cache when valid, but
it cannot mutate the replacement session.

`PublicationIntentId` belongs to the application-wide `PublicationCoordinator`, not to
`ProjectSession`. The coordinator outlives installed project content and orders every atomic file
replacement that can share a target, including project Save, Save As, Save Copy, and frame export.
Session replacement therefore cannot create a same-process write race by erasing publication state.

The application coordinator owns intent IDs, newest-intent registration, and supersession policy.
The distinct `src/platform::StagedArtifactCoordinator` owns canonical target preflight, secure stage
lifetime, same-target publication-lease serialization, atomic replacement, durability operations,
and RAII cleanup. Format adapters use both services through typed requests; neither implements a
private target lock or second intent sequence.

## Session Publication

A replacement is prepared fully off-thread and installed in one bounded authoring-thread step. The
step:

1. advances `SessionResultAcceptanceGeneration` and rejects late result acceptance from the old
   installed content
2. installs the new decoded content, path state, round-trip state, diagnostics, and clean revision
3. constructs a fresh command stack for an editable document
4. clears project-specific selection, hover, interaction overrides, and history
5. for a decoded document, selects the lowest valid `CompositionId` as the initial composition and
   sets exact current time to zero; a project with no composition or preserved read-only content
   exposes no active composition
6. advances preview intent so no old frame can appear
7. publishes one coherent project-changed notification to application projections

Workspace topology, editor types, window geometry, preferences, and recent paths are application or
session preferences and remain unchanged. Observers cannot see a new `Document` paired with the old
command stack or round-trip state.

## New Project

New first passes through the unsaved-change flow. Once allowed, the application constructs and
validates the new document synchronously only when the work is strictly bounded; any template,
media, or scan work remains asynchronous.

The initial snapshot revision is its clean baseline even though no path exists. An untouched new
project therefore closes without an unsaved-change prompt. Its first Save routes to Save As. A
committed edit advances the revision and makes the project dirty.

## Open Intent

Open is a foreground blocking-I/O task. Choosing a path does not mutate the active session or recent
files. Admission performs these steps:

1. resolve any current unsaved-change intent
2. increment the desired `OpenIntentGeneration`
3. capture `ProjectSessionId` and `SessionResultAcceptanceGeneration`, plus document revision for
   decoded content or the immutable content token for preserved read-only content
4. submit a task containing only the chosen path, limits, cancellation/progress channels, and
   captured value identities

A newer Open cancels or supersedes every older queued/running Open for that application session. A
successful worker result installs only when all of these remain true:

- its `OpenIntentGeneration` is still desired
- the captured `ProjectSessionId` and `SessionResultAcceptanceGeneration` still match
- decoded current content still has the revision captured after unsaved resolution, or preserved
  read-only content still has the captured immutable token
- the result is fully validated and has a supported editable or preserved-read-only outcome
- the application is not already in final runtime shutdown

Editing the current project while Open runs is allowed. Such an edit makes the revision check fail;
Bloom keeps the current project and reports that Open was not installed because the current project
changed. It never silently discards the edit and never freezes the authoring UI for background I/O.

Failure, cancellation, supersession, malformed input, unavailable memory, or an unsupported major
version leaves the active document, command history, path, selection, time, preview, and dirty state
unchanged. Only an accepted Open adds its path to recent files.

## Save Inputs And Intent

Save captures a single immutable operation input:

- `ProjectSessionId` and `SessionResultAcceptanceGeneration` for result acceptance only
- document snapshot and exact revision
- immutable round-trip view and preservation capability
- display path, canonical target identity, and expected external-file fingerprint when present
- application-wide `PublicationIntentId` assigned before path resolution
- captured `SessionPathIntentGeneration`; Save As advances it before capture, while plain Save
  observes the current value
- format options, resource limits, cancellation, progress, and diagnostics

The worker does not retain a mutable `Document`, `CommandStack`, `ProjectSession`, Qt object, or raw
panel pointer. Later edits do not change the captured snapshot. Project JSON encoding, compression,
reopen validation, path resolution, flush, and publication run on the blocking-I/O executor and are
visible through Jobs.

Plain Save uses the active path and captures the current `SessionPathIntentGeneration`. A pathless
project routes to Save As. Save As increments that generation when its target is accepted from the
file chooser; it does not change the active path until publication succeeds and result acceptance
still matches that path intent. Therefore a late plain Save to the previous active path cannot
change clean/path state after a newer Save As becomes the accepted path intent.

Editable and degraded-editable projects may save only when Project I/O proves that every retained
unknown member and opaque extension record will survive. A preserved-read-only project cannot run
native Save or Save As; Save Copy stages, validates, and atomically publishes an asynchronous
byte-for-byte copy and never claims to rewrite or migrate the document.

## Per-Target Ordering And Publication

The application-wide `PublicationCoordinator` assigns a strictly increasing, never-reused
`PublicationIntentId` at admission, before asynchronous path resolution, and outlives individual
project sessions. This one sequence covers project saves, byte-preserving Save Copy, and frame
exports. Once a worker resolves a canonical target identity, the coordinator registers that intent
for the target key returned by `src/platform::StagedArtifactCoordinator` preflight. The highest
registered ID wins even when an older request resolves its alias later. Immediately before the
platform coordinator grants its non-cancellable publication lease, the staged artifact must still
own the application coordinator's winning intent for that target.

`PublicationIntentId` is a process-local unsigned 64-bit strong ID in
`1..18446744073709551615`; zero is invalid. Exhaustion rejects new publication admission with a
typed diagnostic and never wraps or reuses an ID. Existing admitted operations retain their order.

Admission returns a move-only `PublicationAdmission` RAII value. Until it either atomically
registers one canonical target or is explicitly abandoned/destroyed, its ID remains in the
coordinator's ordered unresolved set. Registration consumes that unresolved admission and returns
an application target claim that must live through staging, any platform publication lease, and
outcome recording. There is no path that publishes from a bare ID after its admission or target
claim has ended.

For each canonical target, the coordinator stores the highest registered intent and retains the
record as a tombstone after work completes. It also stores active target-claim and publication-lease
counts. Let `lowestUnresolved` be the lowest ID in the ordered unresolved set, or absent when the set
is empty. A target record may be pruned only when it has no active claim or lease and
`lowestUnresolved` is absent or greater than that target's highest registered intent. Consequently,
an older unresolved alias can still find the tombstone and become `Superseded`; after pruning, every
intent that can later register is necessarily newer than the removed high-water value. Target
claims and leases retain their record independently of pruning attempts.

The version 1 application configures hard limits of 256 unresolved admissions and 4096 retained
target records. Capacity is checked before issuing an admission and before consuming an admission
into a new target record. If pruning cannot make room, the operation terminates with
`PublicationTrackingLimit` before staging or replacement; no ID is reused. Admission release,
target-claim release, and lease release each trigger bounded pruning from the global
lowest-unresolved frontier. These limits bound coordinator memory even if path resolution stalls.

If a newer same-target operation has registered, the older stage returns `Superseded` and is cleaned
up without replacing the target. If the older operation already entered its publication lease, it
may complete; the newer operation can enter only afterward. Thus an older artifact never publishes
after a newer admitted artifact has already published.

Precisely, for intents `A < B` that resolve to the same canonical target: if `B` registers before
`A` obtains the platform publication lease, `A` can never obtain that lease; if `A` already holds
the lease, `A` completes before `B` may enter. `A` may already have published while `B` was still an
unresolved alias and therefore not yet known to share the target, but registration of `B` then
preserves the only valid order—older publication followed by newer publication. The inverse order,
older publication after newer publication, is forbidden.

The application coordinator applies one intent order across operation kinds, while the platform
coordinator serializes each canonical target's lease; a project Save and a frame export cannot
bypass each other merely because different services submitted them. Scheduler coalescing is only a
resource optimization. `PublicationIntentId` comparison and `StagedArtifactLease` checks define
correctness across task queues, sessions, Save As aliases, and exporters.

An external-file fingerprint captured by Open or the last accepted save is checked again before
replacement. A mismatch returns `ExternalModificationConflict`. The UI may offer Reload, Save As,
or an explicit Overwrite action; Overwrite creates a new save intent with a fresh expected-target
policy rather than bypassing validation inside the old task.

## Save Result Acceptance

Project I/O reports the publication outcome, captured document revision, resolved target identity,
`PublicationIntentId`, and captured `SessionPathIntentGeneration`. The session accepts state changes
only when the originating `ProjectSessionId`, `SessionResultAcceptanceGeneration`, and path intent
all still match. Save Copy and frame export never propose `ProjectSession` path/dirty changes. These
checks do not decide whether publication succeeded; they decide only whether that truthful result
may update current session state.

| Outcome | File meaning | Session effect when accepted |
| --- | --- | --- |
| `Published` | replacement and requested durability steps succeeded | update path/target/fingerprint and clean revision |
| `PublishedWithDurabilityWarning` | replacement is visible; a later durability step failed | update path/target/fingerprint and clean revision; retain a visible warning and offer Save Again |
| `Superseded` | a newer same-target intent owns publication | no path or clean-revision change |
| `CancelledBeforePublication` | cancellation won before the publication lease | no path or clean-revision change |
| `ExternalModificationConflict` | target no longer matches the expected external state | no path or clean-revision change |
| `FailedBeforePublication` | target was not replaced | no path or clean-revision change |

A publication may succeed after its originating project session was replaced. The file result
remains truthful, but the late result cannot change the new session's path, diagnostics, or dirty
state. It also cannot add a recent path. The application may retain the outcome in Jobs under its
`PublicationIntentId` so the artist can inspect what happened without attributing it to the new
session.

Save deliberately remains valid when the live document advances after capture. If revision `r5`
publishes while the live document is `r6`, the path points to a valid `r5` project and the session
stays dirty. Save As may still become the active path when its path intent is current; its clean
revision is `r5`, not `r6`.

## Dirty State

For a decoded editable document:

```text
dirty = current document revision != clean revision
```

New and Open establish a clean revision. An accepted `Published` or
`PublishedWithDurabilityWarning` result moves it to the published revision. No task progress value,
staged file, validation success, or pre-publication flush changes it.

Revision comparison is intentionally conservative. Undo and redo publish new monotonically
increasing revisions; returning to artistically similar values does not clear dirty state because
allocator high-water or other durable state may still differ. Document revision itself is session
state and is not serialized.

A preserved-read-only project has no mutable dirty state. Its source archive may be copied but not
normalized and presented as a newly saved native document.

## Unsaved-Change State Machine

New, Open, window close, and application quit submit a continuation intent. When the current
editable project is dirty, a UI adapter presents Save, Discard, and Cancel without a nested blocking
event loop:

```text
requested -> awaiting choice
          -> saving -> continue
                    -> awaiting choice after failure or intervening edit
          -> discard -> continue
          -> cancel -> idle
```

- **Save** starts or observes a foreground save for the exact current revision. The continuation
  proceeds only when that revision becomes the accepted clean revision. If the document changes
  while saving, the flow returns to a fresh decision instead of discarding the newer edit.
- **Discard** authorizes the stored continuation, cancels cancelable work owned by the old session,
  and suppresses its late result acceptance into application state. A save already inside its atomic
  publication lease may still finish and must report its real file outcome.
- **Cancel** abandons the continuation and keeps the application and project active.

Save failure, cancellation, conflict, or supersession never becomes implicit Discard. A pathless
project uses Save As; closing a preserved-read-only project needs no unsaved prompt.

Only one destructive continuation is active. Duplicate close/quit requests are idempotent. A later
application-quit request may strengthen a pending window-close continuation, but New/Open actions
remain disabled until the current decision resolves.

## Shutdown

Application close has two stages:

1. resolve unsaved project state while ordinary task admission required by Save remains open
2. after Save/Discard succeeds, stop ordinary admission, cancel replaceable preview/background
   work, detach observers, and await runtime quiescence without blocking the UI event loop

Panels never own or drain the scheduler. Final runtime shutdown cannot begin merely because a
window emitted `closeEvent`; the project-session continuation is authoritative.

## Recent Paths And Presentation State

Recent paths are application preferences, not project truth. Version 1 retains at most ten entries,
most recently accepted first. It adds a path only after successful Open or accepted Save As, removes
duplicates using the resolved target identity, and does not remove a missing path until the artist
chooses to forget it or a later open diagnoses it.

File-dialog directory, window title decoration, workspace layout, editor context, selection, and
current time remain UI/session concerns. They consume project-session state but never enter
`document.json`.

## Required Verification

- Open A then Open B with reverse completion installs only B
- edit during Open, failed Open, cancelled Open, and malformed Open leave all active state unchanged
- successful Open rebinds every projection coherently and clears history, selection, time, and old
  preview publication
- Save revision `r1`, edit to `r2`, and reverse-completing same-target saves retain valid ordering
  and exact dirty state
- Save As A then Save As B cannot let A steal the active path
- session replacement invalidates only `SessionResultAcceptanceGeneration`; an admitted late save
  can publish truthfully without changing path, clean revision, warning, diagnostics, or recents
- a project Save and frame export targeting the same canonical path share one application-wide
  `PublicationIntentId` order and one publication lease
- a lower unresolved alias registering after a higher same-target operation completed sees the
  retained tombstone and is superseded; pruning occurs only beyond the global unresolved frontier
- unresolved-admission and retained-target limits reject excess work before staging without ID
  wrap, reuse, unbounded tombstones, or loss of ordering
- all three session generations reject their exact exhaustion boundary without changing installed
  content, desired Open, or active-path intent
- cancellation at every pre-publication stage and cancellation after publication lease entry report
  different truthful outcomes
- external modification conflict never overwrites without a new explicit intent
- durability warning updates the clean revision while retaining its separate warning state
- session replacement, panel replacement, and quit during slow I/O remain responsive and reject all
  late session mutation
- Save/Discard/Cancel, save failure, pathless Save As, and an edit during unsaved-flow save follow
  the state machine exactly
- recent-path behavior and shared session semantics match on Linux, macOS, and Windows
