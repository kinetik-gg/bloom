# ADR 0018: Application Project Session And Persistence Intent

Status: accepted

Date: 2026-08-25

## Context

Bloom's first interactive slice constructs one `Document`, `CommandStack`, and composition session
for the lifetime of the process. Deterministic project I/O requires stronger ownership: Open must
replace the complete authoring session only after background validation succeeds, Save must publish
the immutable revision the artist requested, and neither a late task nor a panel lifetime may
change the active project accidentally.

Project serialization, compression, validation, path resolution, and durable publication are
latency-variable. The UI must remain usable while they run. At the same time, New, Open, close, and
quit must not discard edits made before or during an asynchronous operation.

## Decision

- Add one Qt-free, application-owned `ProjectSession` for the first proof. It owns the active
  `Document`, `CommandStack`, Project I/O round-trip state, optional path and canonical target
  identity, editability, source format version, diagnostics, and saved-revision baseline.
- Give the runtime service a `ProjectSessionId` and advance a distinct
  `SessionResultAcceptanceGeneration` whenever New or Open replaces installed content. Durable
  `ProjectId` is neither identity. Session replacement invalidates result-driven state mutation,
  not an already admitted platform publication intent.
- Define `SessionResultAcceptanceGeneration`, `OpenIntentGeneration`, and
  `SessionPathIntentGeneration` as independent nonzero unsigned 64-bit counters. Checked exhaustion
  rejects the operation that requires an advance while preserving current state; counters never
  wrap, reset inside a live session, or reuse an issued value.
- Make composition, preview, editor, and scripting sessions projections of the active project
  session. They rebind after a successful replacement and never own project truth or project I/O.
- Run Open, Save, and Save As as foreground blocking-I/O tasks over immutable inputs. Workers never
  retain Qt objects, mutable documents, command stacks, or session references.
- Open installs only a fully decoded and validated project whose request generation is still
  desired and whose replaced session has not changed since the request. Failure, cancellation,
  supersession, or an intervening edit leaves the active project and history untouched.
- Save deliberately publishes its captured snapshot even if later edits exist. The session clears
  dirty state only when the accepted publication reports the live document's exact revision.
- Assign one application-wide, strictly increasing `PublicationIntentId` at admission, before
  asynchronous path resolution, and serialize publication leases per canonical target. The
  application-owned coordinator covers project Save, Save As, Save Copy, and frame export and
  outlives installed project content. A later same-target operation prevents an older staged
  artifact from publishing after it even when aliases resolve out of order. Save and Save As also
  capture `SessionPathIntentGeneration`, which Save As advances, so a late result cannot steal the
  active path or clean state.
- Keep responsibilities split: the application `PublicationCoordinator` owns intent IDs, target
  order, and supersession; `src/platform::StagedArtifactCoordinator` owns canonical target
  preflight, staging lifetimes, publication leases, atomic replacement, durability, and cleanup.
  Project I/O and output adapters create neither private locks nor private intent sequences.
- Make publication admission and target claims move-only RAII values. The application coordinator
  retains per-target high-water tombstones until no stage or lease is active and the global lowest
  unresolved admission is newer than the target high-water. Fixed unresolved-admission and target
  record limits reject excess work before staging. This bounds storage without allowing a late
  lower-ID alias to forget a newer same-target operation.
- Treat atomic publication as non-cancellable once its publication lease begins. Distinguish a
  pre-publication failure, cancellation, or supersession from a successful replacement followed by
  a durability warning.
- Define a new untitled project as a clean revision-zero baseline even though it has no path. Its
  first explicit Save routes to Save As. Every committed revision after the baseline is dirty until
  that exact revision is accepted as published.
- Resolve dirty New, Open, close, and quit requests through a non-blocking Save/Discard/Cancel state
  machine. Runtime shutdown begins only after this resolution; it never closes task admission
  before a requested save can run.
- Keep recent files, file-dialog state, workspace layout, selection, and current time outside
  project truth. Update a recent path only after successful Open or an accepted Save As.
- Freeze project schema `1.0` as a restricted supported-subset encoder. A live
  `DriverBindingSource` fails before staging with typed `UnsupportedDocumentFeature`; it is never
  sampled or discarded. Persist the `driverBinding` allocator high-water even after all driver
  sources are removed so issued IDs are not reused.
- Match serialized composition values to the live domains exactly: positive dimensions no greater
  than `1048576`, checked pixel count no greater than `2^32`, reduced positive `uint32` frame-rate
  and pixel-aspect terms, and a reduced positive rational duration with signed-64 components. Keep
  the exact two-entry ZIP and reference-only OCIO configuration model.
- Make optional node ownership explicit without changing `NodeRecord`: every manifest requirement
  carries a sorted unique `providedNodeTypeIds`, and the lists exactly and singly cover all
  non-foundation node types while excluding foundation types.
- Bound Project I/O through checked per-operation allocation accounting and one shared aggregate
  resident-memory coordinator. Overflow, allocation failure, or budget exhaustion returns a typed
  resource error, preserves the active session, and cannot enter publication.

The detailed lifecycle and acceptance rules are maintained in
[`../architecture/project-session.md`](../architecture/project-session.md). Container bytes,
preservation, limits, and publication outcomes remain owned by
[`../architecture/project-format.md`](../architecture/project-format.md).

## Consequences

- Application wiring must replace permanent references from UI sessions with a rebindable project
  session boundary.
- A decoded Open result can be prepared completely off-thread and installed in one bounded
  authoring-thread step.
- Saving an older captured revision remains honest: the file is valid, while the newer live project
  stays visibly dirty.
- A save may finish publication after its originating installed session was replaced; Jobs reports
  the truthful file outcome, while the mismatched acceptance generation prevents any path, dirty,
  diagnostic, warning, or recent-file mutation in the replacement session.
- For two intents that ultimately resolve to one target, the lower intent may publish first while
  the higher alias is unresolved, but it can never publish after the higher intent. If the lower
  intent already owns the non-cancellable lease, the higher intent waits; otherwise registration of
  the higher intent supersedes the lower before lease entry.
- Closing a window or process becomes an asynchronous intent rather than an immediate scheduler
  shutdown.
- One active project is sufficient for the first proof. Multiple projects, tabs, windows, and
  background project documents remain future product work, but `ProjectSessionId` and result
  acceptance generation do not preclude them.
