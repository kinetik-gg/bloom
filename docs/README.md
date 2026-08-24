# Bloom Documentation

Status: canonical

Updated: 2026-08-25

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
   contract; [`ux/visual-language.md`](ux/visual-language.md) — iconography and typography.
5. [`architecture/overview.md`](architecture/overview.md) — boundaries, state ownership, and
   dependency direction.
6. [`architecture/module-system.md`](architecture/module-system.md),
   [`architecture/layer-graph-model.md`](architecture/layer-graph-model.md),
   [`architecture/animation-and-time.md`](architecture/animation-and-time.md),
   [`architecture/evaluation-primitives.md`](architecture/evaluation-primitives.md),
   [`architecture/task-system.md`](architecture/task-system.md),
   [`architecture/workspace-layout.md`](architecture/workspace-layout.md),
   [`architecture/project-format.md`](architecture/project-format.md),
   [`architecture/project-session.md`](architecture/project-session.md),
   [`architecture/color-management.md`](architecture/color-management.md),
   [`architecture/frame-output.md`](architecture/frame-output.md),
   [`architecture/dependency-intake.md`](architecture/dependency-intake.md),
   [`architecture/scripting-and-addons.md`](architecture/scripting-and-addons.md),
   [`architecture/gpu-backend.md`](architecture/gpu-backend.md), and
   [`architecture/platform-support.md`](architecture/platform-support.md) — execution and platform
   contracts and extension boundaries.
7. [`roadmap.md`](roadmap.md) — proof-oriented implementation sequence.
8. [`decisions/`](decisions/) — accepted architecture decision records.

## Current Checkpoint

- The deterministic CPU preview vertical slice through Batch 3 is implemented and locally verified;
  shared platform CI remains the cross-platform qualification authority.
- Batch 4's rational-time, animation, and interaction contracts are accepted. Durable curves,
  authoring commands, exact sampling, CPU evaluator integration, exact session time, and the
  one-active/one-newest preview-request gate are implemented. The 16 ms pointer cadence, timeline
  key projection, and direct manipulation remain active work.
- Persistence, project-session, color-management, frame-output, and dependency-intake contracts now
  define Batches 5–7. Initial persistence foundations now include bounded I/O memory, canonical
  decimal and Base64 codecs, durable allocator high-water state, opaque extension records,
  application-wide publication ordering, and synchronous Qt-free project-session ownership. JSON,
  ZIP, platform publication, and async Open/Save remain pending. Color and output are accepted
  implementation contracts, including immutable Bloom Neutral v1 as the new-project OCIO default;
  their adapters and qualified dependency profiles remain pending implementation. ADR 0019 accepts
  the dependency mechanism, while concrete prefixes and profiles remain pending qualification.
- Vulkan/MoltenVK remains a working GPU direction until its per-operation parity and lifecycle spike
  passes on Linux, macOS, and Windows.

The detailed implementation status and next merge gates live in [`roadmap.md`](roadmap.md).

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
