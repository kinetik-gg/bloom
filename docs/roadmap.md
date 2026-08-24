# Foundation Roadmap

Status: working

Updated: 2026-08-25

The roadmap is ordered by proof, not by subsystem completeness. A milestone is complete only when its
workflow is demonstrable and its durable behavior is tested.

## M0 — Repository Rebaseline

- Establish C++20, CMake, Qt 6 Widgets, warnings, and a smoke test.
- Make repository documentation canonical.
- Launch a placeholder Compositing workspace based on the UI sketch.
- Establish the quality bar, standards-first policy, task-system contract, GPU direction, and
  cross-platform parity contract.
- Maintain a Linux, macOS, and Windows CI matrix; its first remote execution begins when this root
  is published.

Acceptance: a clean checkout configures, builds, tests, and launches the Qt shell.

## M1 — Document And Command Spine

- Add stable IDs, rational composition time, and diagnostics.
- Model a minimal project, composition, canonical graph, layer facade, and parameters.
- Add typed commands, interaction transactions, undo/redo, and dirty state.
- Connect selection and Properties to read-only document projections.
- Add the task scheduler contract and a non-blocking job/status surface.
- Run a small GPU portability spike on Linux, macOS, and Windows before GPU interfaces harden.

Acceptance: source, transform, opacity, selection, and undo behavior work headlessly and through the
UI without direct panel mutation.

## M2 — First Evaluation Path

- Compile an immutable composition snapshot.
- Evaluate minimal source, transform, composite, and output nodes on CPU.
- Display the evaluated frame in the viewer.
- Add explicit time and resolution requests, cancellation, diagnostics, and conservative caching.
- Keep decode and evaluation asynchronous with no UI-thread waits.
- Add the first accelerated source/transform/composite/output path behind the backend-neutral render
  contract once CPU reference fixtures exist.

Acceptance: the same snapshot and request produce the same image and cache identity.

## M3 — Layer, Node, And Animation Proof

- Present one canonical graph through the node editor and layer timeline.
- Synchronize selection across viewer, nodes, timeline, and Properties.
- Add keyframes, rational-time scrubbing, and direct viewer manipulation.
- Ensure one interaction creates one undoable transaction.

Acceptance: the complete interaction described in `product/foundation.md` works without duplicated
layer and graph truth.

## M4 — Project Round Trip And Output

- Define the first `.bloom` schema and container.
- Implement validation, atomic save, open, and version reporting.
- Import still images and an initial image-sequence representation.
- Render deterministic PNG or EXR output through the CPU reference path.
- Integrate OCIO and OpenEXR/OIIO according to the standards policy rather than creating substitute
  color or professional image semantics.
- Exercise the vertical proof in Linux, macOS, and Windows CI.

Acceptance: save/reopen preserves the evaluated frame and a fixture project renders identically in
headless tests.

## Deferred Until The Proof Is Stable

- editorial sequences and advanced timeline tools
- audio playback and processing
- grading workspace
- scripting and native addons
- OpenFX hosting
- broad interchange and video delivery

GPU breadth and optimization are deferred; the backend contract and a minimal accelerated path are
not.
