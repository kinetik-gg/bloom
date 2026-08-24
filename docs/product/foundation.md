# Product Foundation

Status: working

Updated: 2026-08-25

## Product Identity

Bloom is a native, local-first application for motion, compositing, and editing. Its defining
interaction is that viewer, layers, nodes, media, and properties are synchronized views over shared
project state.

The initial product wedge is compositing and motion. Editorial, grading, scripting, and delivery
remain part of the broader direction but are not commitments for the first implementation proof.

## Accepted Decisions

- Implement Bloom in C++20.
- Use Qt 6 for the native desktop application shell.
- Hold Bloom to the quality and workflow expectations of polished professional motion-graphics and
  VFX desktop software.
- Use established open standards and reference libraries when they cover Bloom's pipeline needs;
  do not create incompatible substitutes for color, image, timeline, or effect interchange.
- Make all workspace content replaceable editor panels. The menu bar is the only fixed application
  surface.
- Use Phosphor Icons for interface iconography, Plus Jakarta Sans for interface typography, and
  Geist Mono for monospaced interface text through native, pinned assets.
- Keep every potentially heavy operation off the UI thread and expose its progress, cancellation,
  failure, and completion state.
- Use GPU acceleration wherever it is correct and beneficial while preserving deterministic CPU
  reference behavior.
- Treat Linux, macOS, and Windows as first-class product targets with equivalent feature semantics.
- Build Bloom as a modular monolith whose optional pipelines can contribute coherent editor,
  document, command, evaluation, render, interchange, and task capabilities without fragmenting
  ordinary in-tree code behind unnecessary abstractions.
- Use Python as Bloom's public artist, pipeline, headless, and add-on language through versioned
  read, command, task, and contribution boundaries. Keep deterministic evaluation native.
- License original Bloom source and documentation under Apache-2.0. Ship community builds with
  self-contained, dynamically linked LGPLv3 Qt and complete third-party compliance records.
- Keep repository documentation as Bloom's canonical current documentation.
- Adopt architectural decisions deliberately through repository documentation and tested behavior.
- Keep the application local-first and capable of headless document and render testing.

## Working Product Model

- A `Project` owns project settings, assets, and authoring containers.
- An `AssetRegistry` owns media identity and interpretation; authoring objects reference stable
  asset IDs rather than raw paths.
- A `Sequence` is an editorial/program container.
- A `Composition` is a visual compositing and motion unit.
- A composition owns one canonical processing graph.
- A native Layer Stack and explicit layer boundaries provide structured layer authoring inside that
  graph; they are not a second render truth.
- Parameters may be static, animated, or driven while retaining one canonical value contract.
- Persistent edits flow through commands so undo, autosave, scripting, validation, and runtime
  invalidation observe the same mutations.
- Time uses explicit rational values and domain-aware mappings. Frame numbers are display labels.

Sequence behavior is retained as a working model, but the first implementation proof does not need
an editorial sequence.

## First Vertical Proof

The first useful Bloom should demonstrate this complete workflow:

1. Create a project and composition.
2. Add a simple source such as a solid, still image, or text item.
3. Represent it coherently in the layer timeline and canonical node graph.
4. Synchronize selection across viewer, timeline, graph, and properties.
5. Edit transform and opacity through properties and direct viewer manipulation.
6. Add keyframes and scrub rational composition time.
7. Undo and redo each persistent change.
8. Save and reopen the project without changing the evaluated result.
9. Render a deterministic PNG or EXR frame.

This proof is the acceptance boundary for the first architecture. Features that do not help prove it
should normally wait.

## Durable Quality Rules

- Stable IDs provide identity; array positions do not.
- Project, session, and derived runtime state remain distinct.
- UI layout and workspace choices never affect render output.
- Evaluation consumes immutable document snapshots.
- A CPU reference path defines correctness before GPU parity is claimed.
- GPU availability, backend, or device differences must not change project semantics.
- Preview degradation must be visible; final rendering must never degrade silently.
- Project files are schema-versioned, validated, and written atomically.
- Image, color, timeline, and effect interchange preserve standard metadata and report unsupported or
  lossy mappings.
- The UI event loop never waits for decode, evaluation, rendering, media scanning, hashing, proxy
  generation, import, export, or external tools.
- A user-visible feature is not complete until it behaves coherently on Linux, macOS, and Windows.
- Project data owned by an unavailable optional module is preserved and diagnosed rather than
  silently discarded.

## Initial Non-Goals

- Full nonlinear editing.
- A DAW-grade audio system.
- Grading and color-finishing workspaces.
- OpenFX hosting.
- Native addon packaging or scripting APIs.
- Network-dependent graph evaluation.
- Video delivery and broad codec coverage.
- Render farms or distributed evaluation.
- Reimplementing a general-purpose UI toolkit.

## Open Product Questions

- Whether Bloom should ultimately be described as motion/compositing-first or as a balanced hybrid
  editor and compositor.
- The exact relationship between sequence clips, compositions, and nested compositions.
- Which source type should anchor the first proof: still image, solid, or text.

## Related Contracts

- [`quality-bar.md`](quality-bar.md)
- [`../standards/strategy.md`](../standards/strategy.md)
- [`../ux/compositing-workspace.md`](../ux/compositing-workspace.md)
- [`../ux/visual-language.md`](../ux/visual-language.md)
- [`../architecture/task-system.md`](../architecture/task-system.md)
- [`../architecture/layer-graph-model.md`](../architecture/layer-graph-model.md)
- [`../architecture/scripting-and-addons.md`](../architecture/scripting-and-addons.md)
- [`../architecture/module-system.md`](../architecture/module-system.md)
- [`../architecture/gpu-backend.md`](../architecture/gpu-backend.md)
- [`../architecture/platform-support.md`](../architecture/platform-support.md)
