# Foundation Roadmap

Status: working

Updated: 2026-08-25

This roadmap is ordered by end-to-end proof and dependency safety. It is the execution plan; the
focused architecture documents own detailed semantics and must not be duplicated here.

## Verified Current Checkpoint

Bloom currently has:

- a C++20 and Qt 6 application shell with a replaceable split-tree workspace
- a Qt-free canonical project, composition, graph, Layer Stack, parameter, and snapshot model
- typed transactional commands with revision-safe undo and redo
- durable composition extent and frame rate, one canonical rational pixel-aspect value shared by
  document and render, versioned node records, and validated finite color values
- exact undoable solid-layer authoring through the same source, Layer Output, and stable Layer Stack
  topology used by other structured layers
- a committed Qt-free bounded task runtime with separate CPU and blocking-I/O executors, semantic
  priority, cancellation, task groups, coalescing, progress, diagnostics, and non-blocking results
- committed Qt-free CPU image contracts for checked extents and layouts, premultiplied `RGBA32F`
  storage, immutable views, exact `lin_rec709_scene` process identity, and prepared packed display
  buffers; Solid authoring metadata remains separately identified
- checked Float32/Float64 scalar primitives with stable IDs and versioned, failure-aware semantics;
  color remains excluded from generic scalar/vector arithmetic
- a frozen startup node-definition registry and deterministic snapshot compiler that follows only
  the graph reachable from the explicit composition output
- an application-owned preview controller that suppresses stale revision/generation results and
  projects honest plan states into replaceable Viewer panels
- a rate-limited Qt task bridge, shared monitor model, and replaceable Jobs editor with stable-ID
  cancellation, progress, duration, terminal history, and complete plain-text diagnostics
- staged application shutdown that intercepts window and application quit paths, keeps processing
  Qt events, publishes final terminal task snapshots, and exits only after runtime quiescence
- a shared composition session projected through Viewer, Timeline, Nodes, Media, and Properties
- atomic text-layer authoring with stable node, layer, slot, edge, and parameter IDs
- strict warnings, formatting, repository hygiene, architecture-boundary checks, and focused local
  tests
- accepted contracts for non-blocking execution, CPU/GPU separation, cross-platform parity, Python
  add-ons, Apache-2.0 distribution, the native `.bloom` container, application project-session
  ownership, and reproducible dependency intake
- exact rational interval factors, typed durable scalar/`Vec2d` curves and keyframes, versioned
  Hold/Linear sampling, animated-parameter compilation/evaluation, and undoable animation authoring
  commands
- exact composition-session time as a non-dirty evaluation input and a preview controller bounded
  to one active plus one newest pending request
- bounded Project I/O memory accounting, allocation-free canonical integer/rational and Base64
  codecs, and a portable streaming SHA-256 identity primitive
- normative Draft 2020-12 manifest and complete writable document `1.0` schema artifacts with
  dependency-free mutation checks
- inclusive allocator high-water preservation across every durable namespace and project-global
  opaque extension envelopes with typed subjects and explicit reference policies
- a bounded application-wide publication coordinator and synchronous Qt-free `ProjectSession`
  ownership with immutable snapshots, command history, clean revision, dirty state, runtime session
  IDs, checked intent generations, sealed path authority, and ordered savepoint callbacks
- a portable, allocation-free process-pixel digest and Process-Frame Semantic Identity version 1
  codec whose canonical bytes exclude execution-provider provenance
- a cancellable frame-bound semantic-identity preparer plus preset-specific immutable PNG/EXR
  preservation reports, stable OutputAnalysis digest golden vectors, preset-bound analysis
  products, and a module-private Output Semantic Identity version 1 streaming serializer/preparer

The local Batch 3 checkpoint is implemented and has been launched. Add Solid remains synchronized
through Timeline, Nodes, Properties, command execution, and undo/redo. Bloom now compiles immutable
composition snapshots, evaluates deterministic reference CPU pixels asynchronously, publishes only
the current revision/generation, and exposes the work through swappable Viewer and Jobs editors.
Batch 4's durable curves, authoring commands, exact sampler, CPU evaluator integration, exact
session time, and one-active/one-newest preview gate are implemented. The 16 ms pointer cadence,
timeline key projection, direct manipulation, and complete project persistence remain the next
implementation slices.

Linux native/core validation is the current automated merge gate. It does not establish desktop or
platform parity. Before a feature is called cross-platform complete, its relevant shared, desktop,
adapter, and packaging suites must also pass qualified Linux, macOS, and Windows environments.

## Execution Order

```text
Batch 1: async/render foundations (three parallel tracks)
             |
             v
Batch 2: snapshot compiler + Jobs + stale-safe preview state
             |
             v
Batch 3: deterministic CPU pixels in Viewer
             |
             v
Batch 4: rational time + animation + direct manipulation
             |
             v
Batch 5: persistence-ready document/platform/dependencies
             |
             v
Batch 6: deterministic .bloom save/open
             |
             v
Batch 7: standards-backed display and PNG/EXR output

GPU qualification starts after Batch 3 and runs beside Batches 4–7.
```

Only one active track may make structural document-schema changes at a time. Runtime, render,
platform, project, and UI work may proceed concurrently when their public seams are frozen and
their source ownership does not overlap.

## Batch 1 — Asynchronous And Render Foundations

Goal: establish the independent contracts needed by every later heavy operation without building a
generic workflow engine or a renderer-shaped task system.

Implementation status: the 1A, 1B, and 1C foundation code and artist-visible Add Solid projection
are implemented and locally verified. The visual checkpoint has been launched for inspection.
Linux native CI protects shared semantics; qualified desktop CI on all three product platforms
remains required for cross-platform completion.

The three tracks were developed independently so their public seams could be reviewed before preview
integration.

### 1A — Bounded Task Runtime

Create the Qt-free task kernel in `src/runtime`:

- strong task and task-group IDs
- semantic `Interactive`, `Visible`, `Foreground`, and `Background` priorities
- independently bounded CPU and blocking-I/O executors
- application, project, composition, panel-request, and export owner scopes represented by stable
  values rather than object pointers
- cooperative cancellation, task groups, structured progress, diagnostics, and terminal outcomes
- typed non-blocking result handles; UI-facing code has no `wait`, blocking `get`, or join path
- coalescing by owner and request key, fair scheduling, backpressure, and bounded terminal history
- completion delivery through a neutral mailbox rather than worker callbacks into Qt
- staged shutdown that rejects new work and reaches quiescence before service destruction

The command stack remains synchronous on the authoring thread. Background tasks never mutate a
Document or invoke widgets.

Suggested commit: `feat: add bounded task runtime`

### 1B — Composition Truth And Solid Authoring

Extend the document and command model with only the durable fields required by evaluation:

- composition extent, square-or-rational pixel aspect, and rational frame rate
- explicit defaults of 1920×1080, square pixels, and 24/1 for a new proof composition
- node schema version in durable node identity; registry lookup uses namespaced type ID plus version
- finite `Color4d`, `bloom.solid-source`, and `bloom.solid.color`
- typed `AddSolidLayer` using the same source → Layer Output → stable Layer Stack topology as other
  structured layers
- commands and validation for format changes

Time remains exact rational seconds; frame numbers are display labels. Widget size never becomes
semantic render resolution.

Text stays authorable but is not faked by the first evaluator. Portable deterministic text requires
font asset identity, resolution, shaping, and rasterization contracts that belong in a later batch.

Suggested commits:

- `feat: add composition render settings`
- `feat: add solid layer authoring`

### 1C — Canonical CPU Image Values

Create `src/render` with Qt-free, allocation-checked values:

- image extent, row layout, data and display windows, pixel aspect, and color-encoding identity
- canonical CPU `RGBA32F` pixels with premultiplied alpha
- explicit immutable ownership and non-owning views
- bounded allocation and overflow diagnostics
- a prepared packed display-buffer result type for later UI handoff

Initial reference pixels use a clearly labelled linear-sRGB encoding. RGB retains finite negative
and HDR values; alpha is finite in `[0, 1]`; transparent canonical pixels have zero RGB. Production
OCIO display processing remains a separate later stage.

Suggested commit: `feat: define CPU image contracts`

### Batch 1 Merge Gate

- Deterministic task tests cover priorities, fairness, pool separation, queue bounds, coalescing,
  cancellation, groups, progress, exception conversion, history bounds, and shutdown.
- Composition values reject zero, overflow, invalid rate/aspect, and hostile extents.
- Solid creation is one exact undoable transaction with stable IDs.
- Image math passes allocation-overflow and alpha/HDR invariants without Qt linkage.
- Strict build, format, hygiene, architecture checks, and CTest pass in Linux native CI. The same
  shared semantics plus desktop/platform suites must pass all three qualified environments before
  the batch is platform-qualified.

Visual checkpoint: launched locally with Add Solid inspectable through synchronized Timeline,
Nodes, selection, and Properties projections. The Viewer states that evaluation is not connected;
no pixel-rendering claim is made.

## Batch 2 — Snapshot Compiler, Jobs, And Preview Lifecycle

Goal: prove that real composition-derived work can execute, report progress, be superseded, and
publish safely before image allocation increases the cost of mistakes.

Implementation status: implemented and locally verified. The visual checkpoint has been launched;
cross-platform parity still requires qualified Linux, macOS, and Windows CI environments.

Parallel work packages:

1. `src/runtime`: a startup-built and then frozen `NodeDefinitionRegistry`, typed port/parameter
   schemas, and deterministic `SnapshotCompiler`.
2. `src/ui`: a `TaskUiBridge`, `TaskMonitorModel`, and replaceable `bloom.jobs` editor. Workers never
   retain a `QObject`.
3. `src/ui` application service: a composition preview controller with owner scope, revision, and
   request generation. It exposes `Preparing`, `Ready`, `Unsupported`, `Cancelled`, and `Failed`
   derived states.

The compiler follows only the graph reachable from the explicit composition output, validates node
type/version, ports, required parameter roles and value kinds, and lowers to a closed typed runtime
operation vocabulary. Type erasure is allowed at registration/compile boundaries, not inside the
hot evaluator.

The Jobs editor is a normal replaceable panel, not a fixed status bar. It shows phase, progress,
priority, state, duration, cancellation, and diagnostics after the initiating panel changes or
closes. Application close uses staged asynchronous shutdown and keeps processing Qt events.

Suggested commits:

- `feat: compile composition snapshots`
- `feat: add jobs editor`
- `feat: manage asynchronous preview requests`

### Batch 2 Merge Gate

- Rapid revisions and same-revision generations suppress every obsolete result.
- Selection-only changes do not compile a graph.
- Replacing Viewer or Jobs during slow fake work is safe and never joins a worker.
- UI progress is rate-limited and delivered only on the UI thread.
- Unsupported reachable nodes carry stable object-scoped diagnostics; unreachable optional nodes do
  not invalidate a valid output path.
- Closing the application with cooperative work in flight remains responsive and reaches clean
  quiescence.

Visual checkpoint: launch Bloom with deliberately observable plan work. Viewer must show only the
current revision while Jobs shows superseded/cancelled generations honestly.

Checkpoint result: the Viewer projects only the application-owned controller's current derived
state and continues to say that no pixels are available. Jobs is a sixth normal editor type and
retains bounded task history when its panel is replaced. Window and application quit paths use the
same non-blocking staged shutdown.

## Batch 3 — Deterministic CPU Composition Preview

Goal: render the first correct pixels through the asynchronous path.

Implementation status: implemented and locally verified on Linux. Qualified shared and desktop CI
on all three product platforms remains required before the batch is called platform-qualified.

Implement:

- a closed immutable plan containing Solid, translation/opacity Layer Output, ordered Layer Stack,
  and Composition Output operations
- explicit evaluation requests containing rational time, output, composition format or proxy
  extent, quality, and color intent
- bounded full-frame CPU evaluation with cooperative cancellation at operation and scanline
  boundaries
- fractional translation with defined bilinear sampling and transparent borders
- premultiplied source-over compositing
- first Layer Stack entry as topmost; evaluation folds from bottom to top
- conservative document-runtime cache identity containing every input that can change pixels
- a worker-side reference linear-sRGB → sRGB packed display mapper with visible `Reference display`
  status
- Viewer publication of already-prepared display pixels with last-good-frame, rendering, stale, and
  failed states

Use two differently colored solids as the main proof fixture. Reordering them must produce a
non-commutatively different result.

Suggested commits:

- `feat: add deterministic CPU compositor`
- `feat: render composition previews asynchronously`

### Batch 3 Merge Gate

- Golden pixel fixtures cover translucent overlap, clipping, fractional translation, opacity 0/1,
  reorder, undo/redo, repeatability, and cache identity.
- Invalid schemas, missing evaluators, unsupported parameter sources, memory pressure, overflow, and
  cancellation produce structured outcomes.
- A slow evaluator proves panel switching, numeric editing, undo/redo, and cancellation keep the Qt
  event loop responsive.
- Stale frames never flash after rapid edits.
- ASan/UBSan render tests and the cross-platform build/test matrix pass.

Visual checkpoint: launch Bloom. Add two solids, edit position and opacity, reorder them, and verify
that Viewer updates asynchronously without blocking.

Checkpoint result: the closed Solid/Layer Output/Layer Stack/Composition Output plan now evaluates
through deterministic `lin_rec709_scene` CPU primitives under an aggregate pixel-memory budget.
The application schedules compile, evaluate, and display mapping as one cancellable task, suppresses
obsolete generations, retains a visibly stale last-good frame, and paints only an immutable prepared
RGBA8 handoff. The latest local build is launched for artist visual QA; subjective appearance remains
outside the automated merge gate.

## Batch 4 — Rational Time, Animation, And Direct Manipulation

Goal: complete the time-dependent authoring part of the first vertical proof and make the document
ready for a stable project schema.

Contract status: accepted in ADRs 0016 and 0017 and
[`architecture/animation-and-time.md`](architecture/animation-and-time.md). Durable IDs, curve
validation, sampling, commands, exact session time, and the one-active/one-newest request gate are
implemented. The 16 ms pointer cadence, timeline projection, and direct gestures remain pending;
cubic curves and shared animation are explicitly outside this batch.

Implement in dependency order:

1. durable keyframe IDs, animation-curve storage, interpolation modes, validation, and allocator
   support
2. typed insert/update/delete keyframe commands and gesture transactions
3. exact parameter sampling at rational request time in the CPU evaluator
4. composition-session current time, playhead/scrub behavior, and timeline keyframe projection
5. direct Viewer translation using one command transaction per gesture
6. asynchronous coalescing so rapid scrubbing publishes only the newest requested frame

Start with scalar and `Vec2d` animation needed by opacity and position. Do not design every future
curve modifier or expression feature in this batch.

Suggested commits:

- `feat: add animation curve authoring`
- `feat: evaluate animated parameters`
- `feat: add timeline scrubbing and keyframes`
- `feat: add viewer transform interaction`

### Batch 4 Merge Gate

- Curve storage survives snapshot, commit, undo, and redo with exact IDs and rational times.
- One drag creates one understandable undo step.
- Scrub storms remain bounded and never publish an old frame.
- Timeline, Viewer, Nodes, and Properties retain one primary/contextual selection truth.

Visual checkpoint: animate position/opacity, scrub the playhead, directly move the selected layer,
and undo each gesture.

## Batch 5 — Persistence-Ready Foundations

Goal: implement the accepted wire, durable-state, platform-publication, and dependency-intake
contracts needed by deterministic project I/O.

Contract status: ADRs 0015, 0018, and 0019 accept the container, project-session intent, and
offline-superbuild/lock/qualified-prefix mechanism. Foundational I/O memory, integer/rational and
Base64 codecs, canonical JSON strings/layout, canonical manifest encoding/schema checks, manifest
requirement coverage, durable allocator/extension state, publication ordering, budget-enforced PMR
allocation, a Linux staged-artifact foundation with mandatory close/reopen verification, standalone
color-setting values/content revisions, and synchronous project-session ownership are implemented.
Complete document JSON/ZIP I/O, unknown-member overlay, format-specific semantic verification,
cross-platform filesystem publication, async session task orchestration and atomic content install,
Project-owned color identity, the qualified built-in asset, and all concrete dependency profiles
remain pending; the core session ID, generation, path-authority, and savepoint-acceptance seams are
implemented. No dependency is qualified merely because the intake mechanism is accepted. See
[`architecture/project-format.md`](architecture/project-format.md),
[`architecture/project-session.md`](architecture/project-session.md), and
[`architecture/dependency-intake.md`](architecture/dependency-intake.md).

Parallel implementation tracks:

### 5A — Format Schema, Codec Surface, And Fixtures

Implementation status: canonical object-ID, allocator, signed-integer, JSON-`uint32`, normalized
rational, positive-ratio, strict padded Base64, UTF-8 JSON-string, and bounded canonical-layout
primitives with shared exact count/write state are implemented with adversarial tests. Canonical
manifest encoding, its Draft 2020-12 schema artifact, and manifest requirement shape/order and
exact project coverage validation are also implemented. The complete writable document `1.0`
Draft 2020-12 schema artifact, strict artifact checker, exact canonical Float64 codec, finite
known-field Float64 normalization, and the lossless editable unknown-number subset are implemented.
DOM integration, the document writer, instance validation, and golden document fixtures remain
pending.

- implement the accepted complete `manifest.json` and `document.json` shapes and JSON Schema
  dialect without reopening their wire semantics
- encode the accepted requirements, array ordering, integer/negative-zero/rational forms, extension
  payload bytes, resource limits, schema-version agreement, symlink policy, and canonical target
  identity in schemas and typed codec tests
- add readable golden fixtures and an adversarial corpus

### 5B — Persistence-Ready Document State

Implementation status: complete inclusive allocator high-water state and opaque extension envelopes
are implemented and preserved through document snapshot, draft, commit, removal, and restore.

- expose validated durable-ID allocation high-water state for every namespace, including keyframes
  and extension records
- restore allocator state exactly so deleted IDs cannot be reused after reopen
- add opaque extension records with stable record ID, namespaced owner/type, schema version, optional
  typed subject, media type, and payload bytes

### 5C — Platform And Dependency Foundation

Implementation status: the bounded application `PublicationCoordinator` and a tested Linux
preflight/staging/atomic-publication foundation are implemented. The platform lease now requires a
checked writer close, reopens the exact stage no-follow, exposes bounded verification reads, and
blocks publication until explicit verifier acceptance. Format-specific verifiers,
cancellation/progress hooks, macOS and Windows providers, parity fixtures, safe lifetime-record
pruning integration, and concrete dependency profiles remain pending.

- pin yyjson, libzip, zlib, and hashes; keep them private/SYSTEM and record licenses/SBOM metadata
- add a narrow staged-file/atomic-publication interface with POSIX, macOS, and Windows behavior and
  fault-injection seams
- implement the bounded application `PublicationCoordinator`, including admission RAII,
  lowest-unresolved tombstone pruning, and shared ordering across Project I/O and frame output

### 5D — Durable Color Identity And Built-In Asset

Implementation status: the standalone `ColorSettings`/`OcioConfigReference` v1 value model,
locator/context validation, and versioned built-in/archive and external-loose revision hashing are
implemented. No placeholder digest is manufactured. Attaching the values to `Project`, creating
new-project defaults, and snapshot preservation wait for the qualified Bloom Neutral asset/profile
so those changes can land without a false identity.

- add project-level `ColorSettings` and `OcioConfigReference` to durable document state, with new
  projects fixed to `lin_rec709_scene` and `bloom://ocio/neutral-v1/config.ocio`
- provision the immutable Bloom Neutral v1 asset and its exact revision digest through the qualified
  build/package profile; a digest mismatch or mutable alias fails the build/package gate
- keep OCIO parsing, processor creation, display transforms, and output transforms out of this batch;
  they remain Batch 7 work over the already-durable identity

Suggested commits:

- `feat: add project schema and codec fixtures`
- `feat: make document state persistence-ready`
- `build: pin project format dependencies`
- `feat: add atomic file publication`
- `build: provision Bloom Neutral color config`

### Batch 5 Merge Gate

- Deleted IDs remain unavailable after exact allocator reconstruction.
- Animation and extension records survive snapshot/draft/commit.
- Atomic-publication tests distinguish pre-publication failure from post-publication durability
  warning.
- Late path aliases cannot regress publication order, and coordinator record limits fail before
  staging without wrap or unbounded tombstones.
- New documents contain the exact Bloom Neutral v1 URI and qualified digest, and packaged asset
  bytes reproduce that digest on Linux, macOS, and Windows.
- Pinned dependency builds and platform adapters compile in all three CI environments.

## Batch 6 — Deterministic `.bloom` Save And Open

Goal: save/reopen without changing project truth or blocking the UI.

Contract status: persistence and Qt-free `ProjectSession` behavior are frozen in
[`architecture/project-format.md`](architecture/project-format.md) and
[`architecture/project-session.md`](architecture/project-session.md). Synchronous ownership,
runtime identity/generation admission, and ordered savepoint acceptance are implemented; project
container I/O and asynchronous application orchestration remain pending.

Land in reviewable increments:

1. strict bounded JSON parser/writer, duplicate-key rejection, migrations, move-only unknown-member
   round-trip state, schemas, deterministic golden bytes, and exact durable `ColorSettings`
   round-trip
2. constrained ZIP reader/writer with entry/path/attribute/CRC/resource validation and no extraction
3. same-directory stage, close, reopen/decode validation, flush, then non-cancellable atomic
   publication
4. an application-owned project session containing Document, Command Stack, round-trip state, path,
   editability, and saved revision
5. File → New/Open/Save/Save As, dirty state, unsaved-change flow, recent path, and actionable
   degraded/read-only state, all through foreground I/O tasks

Suggested commits:

- `feat: add deterministic project json codec`
- `feat: add Bloom project container`
- `feat: add asynchronous project service`
- `feat: connect project save and open workflow`

### Batch 6 Merge Gate

- Same snapshot produces identical canonical `document.json` bytes.
- IDs, allocator watermarks, integer extrema, exact finite doubles, Unicode, rationals, animation,
  graph order, and extension payloads round-trip.
- Unknown additive members survive stable-ID edits; unknown core discriminators open read-only.
- Traversal, duplicate entries, malformed UTF-8, CRC corruption, executable/symlink entries, and zip
  bombs are rejected.
- Failed open leaves the active project, selection, and history unchanged.
- For two saves resolving to one canonical target, the lower intent may publish while the higher
  alias is still unresolved, but it can never publish after the higher intent; dirty state clears
  only for the exact revision accepted on disk.
- Slow save/open leaves the UI responsive and inspectable through Jobs.
- Shared fixtures round-trip on Linux, macOS, and Windows.

Visual checkpoint: save the animated composition, close/reopen it, and verify the evaluated frame,
selection reset, animation, and layer/node structure.

## Batch 7 — Standards-Backed Display And Output

Goal: complete the first proof with professional color-managed presentation and deterministic file
output rather than a private image writer.

Contract status: the v1 `lin_rec709_scene` process identity, immutable Bloom Neutral v1 new-project
default, project-qualified OCIO boundary, preservation analysis, and semantic-determinism
PNG/flat-EXR publication contracts are accepted implementation contracts in
[`architecture/color-management.md`](architecture/color-management.md) and
[`architecture/frame-output.md`](architecture/frame-output.md). Dependency implementations and
qualification remain pending. The portable Process Pixel Stream and Process-Frame Semantic Identity
version 1 codecs, owning preparer, preset-specific preservation analyzer/report, analysis digest,
preset-bound analysis products, the module-private Output Semantic Identity version 1 streaming
serializer/preparer, and golden vectors are implemented; preset conversion, production
verifier-product issuance, file adapters, and semantic verification remain pending.

Implement:

- resolve the already-durable, qualified OpenColorIO configuration identity; create CPU display
  processors and explicit missing-config diagnostics
- standards-backed PNG and flat OpenEXR output through qualified OpenImageIO/OpenEXR components
- explicit alpha, color, data/display window, pixel aspect, channel, and metadata behavior
- cancellable foreground render tasks using immutable snapshots and explicit quality/color intent
- export capability/loss reporting even for the initially small supported subset

Suggested commits:

- `feat: add color-managed CPU display`
- `feat: render PNG and OpenEXR output`

### Batch 7 Merge Gate

- Viewer and output color intent are explicit and tested; preview compromises remain labelled.
- Fixed HDR/negative/alpha fixtures survive the declared output subset.
- Save/reopen preserves the same evaluated frame and headless output identity.
- Final output never silently falls back to preview quality or missing color processing.

Visual checkpoint: reopen a project, select an OCIO view, and render a deterministic PNG or EXR.
Completing this gate completes the first vertical proof in `product/foundation.md`.

## Parallel GPU Qualification Lane

Begin the gated Vulkan/MoltenVK spike only after Batch 3 supplies CPU fixtures, task scheduling, and
request semantics. It may run beside Batches 4–7 under separate `src/render` backend files and
dependency manifests.

The spike must not change document semantics or replace the CPU final path. Promotion requires the
cross-platform capability, parity, cancellation, presentation, memory-pressure, shader-failure, and
device-loss gates in [`architecture/gpu-backend.md`](architecture/gpu-backend.md). Until then the
GPU direction remains working rather than accepted.

## Agent Workload Protocol

Each execution wave uses root plus up to three agents:

1. Root freezes the smallest public seam, assigns non-overlapping source ownership, and reserves
   top-level CMake, app composition, shared registries, and canonical docs for integration.
2. Agents work in module-local files and do not commit. One agent owns implementation, another may
   own independent adversarial tests, and a third owns a separate module/UI projection.
3. Root reviews boundary direction and failure behavior, stages exact paths, runs the full gate, and
   creates focused Conventional Commits.
4. A failed or unclear boundary is corrected before downstream agents build more code on it.
5. A new build is launched at the visual checkpoints above, not for invisible internal-only
   commits.

Two agents do not concurrently edit the same document schema, CMake file, registry, or application
composition root. Parallel speed comes from stable boundaries, not conflict-heavy shared edits.

## Quality Gate For Every Merge

- warnings-as-errors and `clang-format` pass
- repository hygiene and architecture-boundary checks pass
- focused unit tests and the full CTest suite pass
- no Qt types enter core, document, commands, host, render, runtime, project, color, output, or
  platform contracts
- no UI callback waits on futures, workers, I/O, GPU work, or task shutdown
- no generic service locator, `std::any` result bus, duplicated project truth, or speculative public
  ABI enters the slice
- active production files remain cohesive and normally below the repository's 700-line review
  threshold
- ASan/UBSan cover image/project parsing paths; TSan covers scheduler and publication services where
  available
- Linux, macOS, and Windows shared semantics remain green before a feature is called complete

## Deferred Until The First Proof Is Stable

- deterministic text shaping/rasterization and font asset management
- still/video ingest, broad codecs, and audio; the provider, isolation, qualification, and ProRes
  constraints are researched in [`architecture/media-io.md`](architecture/media-io.md) without
  moving implementation ahead of the first-proof gate
- masks, mattes, effects, blend-mode breadth, adjustment layers, and nested compositions
- full nonlinear editing, grading, and delivery workspaces
- public Python/add-on runtime implementation and custom PySide UI
- OpenFX hosting
- persistent render caches, distributed rendering, and render farms
- a stable public native plug-in ABI

These are deliberate deferrals, not permission for the current boundaries to make them impossible.
