# Bloom Documentation

Status: canonical

Updated: 2026-10-09

This directory is Bloom's current source of truth. Historical notes outside this repository may
provide research or rationale, but they are not binding until adopted here.

## Reading Order

1. [`product/foundation.md`](product/foundation.md) — product identity, current decisions, v0 proof,
   and non-goals.
2. [`product/quality-bar.md`](product/quality-bar.md) — professional artist experience, reliability,
   responsiveness, and VFX expectations; [`product/node-catalogue.md`](product/node-catalogue.md) —
   planned artist-facing node families and delivery levels.
3. [`standards/strategy.md`](standards/strategy.md) — standards-first pipeline and interoperability
   policy.
4. [`ux/compositing-workspace.md`](ux/compositing-workspace.md) — the UI sketch as an interaction
   contract; [`ux/visual-language.md`](ux/visual-language.md) — iconography and typography;
   [`ux/interaction-model.md`](ux/interaction-model.md) — the keyboard and pointer bindings,
   including blank-project startup and the New Composition duration units.
5. [`architecture/overview.md`](architecture/overview.md) — boundaries, state ownership, and
   dependency direction.
6. [`architecture/module-system.md`](architecture/module-system.md),
   [`architecture/layer-graph-model.md`](architecture/layer-graph-model.md),
   [`architecture/value-node-library.md`](architecture/value-node-library.md),
   [`architecture/animation-and-time.md`](architecture/animation-and-time.md),
   [`architecture/evaluation-primitives.md`](architecture/evaluation-primitives.md),
   [`architecture/task-system.md`](architecture/task-system.md),
   [`architecture/workspace-layout.md`](architecture/workspace-layout.md),
   [`architecture/project-format.md`](architecture/project-format.md),
   [`architecture/project-session.md`](architecture/project-session.md),
   [`architecture/color-management.md`](architecture/color-management.md),
   [`architecture/frame-output.md`](architecture/frame-output.md),
   [`architecture/media-io.md`](architecture/media-io.md),
   [`architecture/dependency-intake.md`](architecture/dependency-intake.md),
   [`architecture/scripting-and-addons.md`](architecture/scripting-and-addons.md),
   [`architecture/gpu-backend.md`](architecture/gpu-backend.md), and
   [`architecture/platform-support.md`](architecture/platform-support.md) — execution and platform
   contracts and extension boundaries.
7. [`roadmap.md`](roadmap.md) — proof-oriented implementation sequence.
8. [`decisions/`](decisions/) — accepted architecture decision records.

## User Guides

- [Organizing Assets](user-guide/assets.md) — folders, display names, tags, search and drag order.
- [Import from network shares](user-guide/network-shares.md) — mounting an SMB share and
  importing from it on Linux, macOS and Windows.
- [Preferences](user-guide/preferences.md) — global preferences, which changes take effect immediately,
  which need a restart, and what is project or session state instead.
- [Memory](user-guide/memory.md) — the cache budgets Bloom chooses, the two override keys, and what
  "Memory pressure: caches trimmed" means.
- [Scripting Bloom with Python](user-guide/python.md) — Python package, transactions and embedding.
- [Using Bloom from an agent](user-guide/mcp.md) — stdio MCP tools and request limits.
- [Headless scripting](user-guide/scripting.md) — `bloom-cli`, JSON transactions, and headless
  frame/range rendering.

## Current Checkpoint

- The deterministic CPU preview vertical slice through Batch 3 is implemented and locally verified;
  qualified shared and desktop CI on all three product platforms will remain the cross-platform
  qualification authority. The current automated merge gate is Linux native/core only.
- Batch 4's rational-time, animation, and interaction contracts are accepted. Durable curves,
  authoring commands, exact sampling, CPU evaluator integration, exact session time, and the
  one-active/one-newest preview-request gate are implemented. The 16 ms pointer cadence, timeline
  key projection, and direct manipulation remain active work.
- Persistence, project-session, color-management, frame-output, and dependency-intake contracts now
  define Batches 5–7. Initial persistence foundations now include budget-enforced I/O allocation,
  strict text/decimal/Base64 primitives, bounded canonical JSON layout writing, canonical manifest
  encoding and schema checks, manifest dependency validation, durable allocator and extension
  state, application-wide publication ordering, a Linux staged-artifact foundation with mandatory
  close/reopen verification, and synchronous Qt-free project-session ownership. Complete JSON
  parsing/document encoding, the document schema, ZIP, format-specific semantic verification,
  non-Linux publication, and async Open/Save remain pending. Standalone durable color-setting
  values and versioned OCIO content-revision primitives are implemented without inventing a default
  digest; Project ownership waits for the qualified Bloom Neutral v1 asset and profile. ADR 0019
  accepts the dependency mechanism, while concrete prefixes and profiles remain pending
  qualification.
- A shared GPU scene prepares every current compiled pixel operation under a strict fail-closed
  operation and render-route coverage contract, with no pixel exception. The resident preview and
  final-render routes both evaluate through it, including the general GPU output-colour production
  context, a 6000x4000 media+CST+text export with locally verified native execution, and
  capacity-aware resident sizing. The CPU reference evaluator remains the correctness oracle and
  explicit supported fallback; GPU execution is locally exercised on Linux only, and macOS and
  Windows have an explicit supported CPU fallback. Vulkan/MoltenVK remains a working GPU direction
  until its cross-platform parity and lifecycle spike passes.
- Ordinary Viewer preview now uses genuine GPU scene/display preparation followed by an asynchronous
  final RGBA8 transfer and QWidget painting, explicitly labelled GPU-prepared/CPU-painted. Bounded
  private Linux checks qualify clipping, ROI, coherent outlines, host-only Full/Half 100-frame reuse,
  real animated text manipulation and short useful cold progress. Channel remapping is a bounded
  CPU worker with explicit provenance. Native preview/final GPU coverage remains required. These
  checks do not establish sustained playback or cross-platform GPU qualification; broader scheduling,
  clock, media, aggregate memory and batching work remains in the
  [GPU reliability plan](plans/gpu-reliability-and-preview.md).
- Shared preview completion now uses callback-free neutral runtime subscriptions and one coalesced
  queued UI drain. RAM/background refill and typed Viewer/asset/bootstrap/export/purge consumers
  receive completed results independently of Jobs progress snapshots. Bounded private tests qualify
  burst coalescing, held-worker UI activity and GPU Half100 refill; native acknowledgements,
  separate text-layout/ProjectHost I/O polling and sustained/platform gates remain separate.
- Playback now derives cache-independent targets from elapsed rational wall/audio time, using checked
  valid-grid origins, live reanchoring and absolute intent-time deadlines. Five competing 60-second
  CPU-reference sessions show zero exact desired-grid mismatches; private GPU animation pixels
  separately qualify short visible progress. This is clock/image correctness, not painted FPS or
  real-audio/sustained/platform qualification.

- Reusable cache retention now uses off-UI process-aware sampling, pairing visible Linux cgroup v2
  limits/usage with conservative reserve and zero reclaimable credit. Cold zero retention preserves
  foreground preparation and separate live export reservations. Saved requests, configured ceilings
  and effective reusable allowances remain distinct; unsupported platforms disclose host-only fallback.
  Bounded policy/lifetime tests and a private capped helper pass. This is not an aggregate process/OOM
  guarantee. Broader reservations and remaining G4–G10 gates stay open.

- Typed task results now belong to live handles rather than retained terminal history. Weak writers,
  admission pins and pointer-only lock exchange preserve take-once delivery while releasing abandoned
  results outside locks. CPU/I/O and mock GPU forced/late/reentrant lifetime gates pass; scheduler
  history, accounting and completion ordering remain unchanged. The first G6B phase-reservation slice is accepted separately;
  this adds no native GPU performance or whole-process memory guarantee.

- The first conservative G6B processing-phase reservation slice is accepted within bounded scope:
  explicit shared roots, checked host/device/count envelopes, admitted CPU fallback,
  retirement/quarantine lifetime and bounded readiness/capacity-release retry. Required native
  coverage/routes, affected strict checks, application quality and Linux CPU-only closure pass,
  including corrected owning reruns; the reliability plan records their distinct evidence.
  Allocation-owned physical accounting, width two and sustained/platform qualification remain open;
  see the [owning GPU contract](architecture/gpu-backend.md#conservative-processing-phase-reservations-first-g6b-slice).

The detailed implementation status and next merge gates live in [`roadmap.md`](roadmap.md).

See [Export](user-guide/exporting-video.md) for composition export presets, audio,
verification and cancellation.

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
