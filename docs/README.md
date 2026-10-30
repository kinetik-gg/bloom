# Bloom Documentation

Status: canonical

Updated: 2026-10-30

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
   including blank-project startup and the New Composition duration units;
   [`ux/sound.md`](ux/sound.md) — UI sound feedback, render notifications, and the event audit.
5. [`architecture/overview.md`](architecture/overview.md) — boundaries, state ownership, and
   dependency direction.
6. [`architecture/module-system.md`](architecture/module-system.md),
   [`architecture/layer-graph-model.md`](architecture/layer-graph-model.md),
   [`architecture/value-node-library.md`](architecture/value-node-library.md),
   [`architecture/animation-and-time.md`](architecture/animation-and-time.md),
   [`architecture/layer-alignment.md`](architecture/layer-alignment.md),
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
   [`architecture/gpu-backend.md`](architecture/gpu-backend.md),
   [`architecture/platform-support.md`](architecture/platform-support.md), and
   [`architecture/ui-feedback.md`](architecture/ui-feedback.md) — execution and platform
   contracts and extension boundaries.
7. [`roadmap.md`](roadmap.md) — proof-oriented implementation sequence.
8. [`decisions/`](decisions/) — accepted architecture decision records.

The issue #304 Linux implementation and local qualification cover processing/presentation allocation
ownership, immutable source revisions, bounded native command batches, repeated fill/purge and
sustained packed Viewer playback. Admission now uses measured capacity minus the RAM/VRAM headroom
preferences and each frame's actual need, and service-backed RAM fill keeps as many frames in flight
as fit (see [GPU admission](architecture/gpu-backend.md#processing-phase-admission-by-measured-capacity-and-actual-need)). See the
[current Linux qualification record](plans/gpu-304-linux-qualification.md) and the historical
[execution checkpoints](plans/gpu-reliability-and-preview.md#execution-checkpoint) for exact limits.

## User Guides

- [The Export panel](user-guide/exporting-video.md) — the render queue, outputs, destinations and replacing files.
- [Organizing Assets](user-guide/assets.md) — folders, display names, tags, search and drag order.
- [Import from network shares](user-guide/network-shares.md) — mounting an SMB share and
  importing from it on Linux, macOS and Windows.
- [Preferences](user-guide/preferences.md) — global preferences, which changes take effect immediately,
  which need a restart, and what is project or session state instead.
- [Memory](user-guide/memory.md) — the RAM and VRAM headroom preferences, how work and caches use
  the rest of the machine, and what actual OS limits apply.
- [The Context Bar](user-guide/context-bar.md) — the tools, the active tool's options, and aligning
  and distributing layers.
- [Workspace presets](user-guide/workspace-presets.md) — the Workspace picker, your own layouts and how the Export panel opens.
- [The status bar](user-guide/status-bar.md) — the labelled groups along the bottom of the window.
- [Scripting Bloom with Python](user-guide/python.md) — Python package, transactions and embedding.
- [Using Bloom from an agent](user-guide/mcp.md) — stdio MCP tools and request limits.
- [When a composition cannot render](user-guide/compile-failures.md) — the status bar's Details popup and what to do.
- [MCP Server panel](user-guide/mcp-server-panel.md) — start or stop the in-app MCP server, port, token and clients.
- [Headless scripting](user-guide/scripting.md) — `bloom-cli`, JSON transactions, and headless
  frame/range rendering.

## Current Checkpoint

- The UI kit polish release is complete: one control look and height, the shared popup window, the
  Inter/Geist type system, the contextual tool bar ([ADR 0031](decisions/0031-contextual-tool-bar.md),
  [user guide](user-guide/context-bar.md)) and a UI grammar gate with an empty allowlist. What
  shipped, the validation evidence and the macOS/Windows gaps are in the
  [release record](plans/ui-kit-polish-release.md).
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
  Linux qualification now includes ten Half100 fill/purge cycles, 60 seconds of retained painted
  playback and bounded command batching in the [current record](plans/gpu-304-linux-qualification.md).
  Cross-platform native execution, physical scanout and whole-process memory guarantees remain
  unqualified.
- Shared preview completion now uses callback-free neutral runtime subscriptions and one coalesced
  queued UI drain. RAM/background refill and typed Viewer/asset/bootstrap/export/purge consumers
  receive completed results independently of Jobs progress snapshots. Bounded private tests qualify
  burst coalescing, held-worker UI activity and GPU Half100 refill; native acknowledgements,
  separate text-layout/ProjectHost I/O polling and platform gates remain separate. The Linux
  sustained packed playback result is recorded above.
- Playback now derives cache-independent targets from elapsed rational wall/audio time, using checked
  valid-grid origins, live reanchoring and absolute intent-time deadlines. Five competing 60-second
  CPU-reference sessions show zero exact desired-grid mismatches; private GPU animation pixels
  separately qualify short visible progress. This is clock/image correctness, not painted FPS or
  real-audio/sustained/platform qualification.

- Reusable cache retention now uses off-UI process-aware sampling, pairing visible Linux cgroup v2
  limits/usage with zero reclaimable credit. Explicit retention preferences are authoritative;
  only observed absolute OS limits constrain their combined allowances. Cold zero retention preserves
  foreground preparation and separate live export reservations. Saved requests, configured ceilings
  and effective reusable allowances remain distinct; unsupported platforms disclose host-only fallback.
  Bounded policy/lifetime tests and a private capped helper pass. This is not an aggregate process/OOM
  guarantee. Broader reservations and remaining G4–G10 gates stay open.

- Typed task results now belong to live handles rather than retained terminal history. Weak writers,
  admission pins and pointer-only lock exchange preserve take-once delivery while releasing abandoned
  results outside locks. CPU/I/O and mock GPU forced/late/reentrant lifetime gates pass; scheduler
  history, accounting and completion ordering remain unchanged. Processing-phase admission is
  documented separately and adds no whole-process memory guarantee beyond the headroom.

- Processing-phase admission uses measured capacity minus the RAM/VRAM headroom preferences
  (default 2 GiB each) and each phase's actual need: explicit shared roots, checked host/device/count
  claims, typed waits that resume on release, refusal only when one frame cannot fit the whole usable
  capacity, strict Viewer priority for exports, retirement/quarantine lifetime and recovery of a
  generation latched by an unproven retirement; see the
  [owning GPU contract](architecture/gpu-backend.md#processing-phase-admission-by-measured-capacity-and-actual-need).

- Rooted packed-host RGBA8 payload ownership and conservation are accepted within
  [a separate bounded slice](architecture/gpu-backend.md#rooted-packed-host-payload-ownership-bounded-ownership-slice).
  Its off-UI published-retirement follow-on implements charged worker freeing with explicit
  shutdown ownership; core/GUI, headless, Linux CPU-only and all 103 affected strict checks pass.
  No whole-process accounting is implied.

The detailed implementation status and next merge gates live in [`roadmap.md`](roadmap.md).

The [performance and reliability sprint](plans/performance-reliability-sprint.md) tracks the ordered
node, recovery, RAM preview, cache, audio, media and document work under #306, with
[local milestone validation](development/local-validation.md).

The [production workflows release](plans/production-workflows-release.md) records what landed under
#374 (timeline, footage, masking, EXR, composition settings and Assets) and the work still open.

See [The Export panel](user-guide/exporting-video.md) for the render queue and
[Export presets](user-guide/exporting-video.md) for presets, audio, verification and cancellation.

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
