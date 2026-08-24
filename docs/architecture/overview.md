# Architecture Overview

Status: working

Updated: 2026-08-25

## Objective

Bloom's architecture should make the first compositing workflow small while preserving clear paths
to deterministic rendering, professional media handling, and later editorial functionality.

The architecture is organized by ownership and dependency direction, not by UI panels or third-party
libraries.

Artist-facing quality, non-blocking execution, standards interoperability, GPU acceleration, and
cross-platform parity are architectural constraints from the beginning.

Bloom is a modular monolith. The application is composed from owned modules and explicit optional
pipeline contributions, but known in-tree modules use direct typed APIs instead of paying dynamic
plug-in costs at every call.

## State Categories

### Project Truth

Saved, command-mutated authoring data:

- assets and their stable identities
- compositions and future sequences
- layers and canonical graphs
- parameters and animation
- project settings and render intent

### Session State

Replaceable interaction state:

- active composition and editor context
- selection and hovered item
- current time and playback controls
- active tool and interaction transaction
- workspace layout, panel type, zoom, and scroll positions

Some session preferences may be persisted, but they must not influence evaluation semantics.

### Derived Runtime State

Rebuildable state:

- compiled evaluation plans
- decoded media frames
- CPU and GPU image resources
- caches, thumbnails, proxies, and waveforms
- profiling and diagnostic state

Derived state is never the source of authoring truth.

## Dependency Direction

```text
Application Composition Root / Module Catalog
        |
        +---------------------> Optional Pipeline Modules
        |                              |
        |                         typed registries
        v                              v
Qt Application / Replaceable Editor Panels
        |
        | read models and issue commands
        v
Application Session ---------> Command Stack
        |                             |
        | schedule                    v
        +---------------------> Task System
                                      |
                    immutable inputs | results / diagnostics
                                      v
                                Document Model <----> Project I/O / Standards Adapters
                                      |
                                      | immutable snapshot
                                      v
                              Evaluation Runtime
                                      |
                                      v
                            CPU / GPU Render Backends
```

Forbidden dependencies include:

```text
Document -> Qt UI
Document -> GPU or platform APIs
UI callback -> raw mutable document
UI thread -> heavy work or blocking wait
Runtime cache -> authoring truth
Workspace layout -> render result
Platform/GPU capability -> project semantics
```

## Initial Source Boundaries

| Area | Responsibility |
| --- | --- |
| `apps/bloom` | process entry point and final service wiring |
| `src/host` | application services, compiled-in module catalog, dependency validation, registries |
| `src/ui` | Qt shell, editor panels, view models, interaction adapters |
| `src/core` | IDs, rational time, diagnostics, math, small value types |
| `src/document` | persistent project authoring model and validation |
| `src/commands` | commands, transactions, undo/redo, dirty state, events |
| `src/project` | schemas, migrations, atomic `.bloom` read/write |
| `src/runtime` | task scheduling, snapshot compilation, evaluation, cache contracts |
| `src/render` | image resources, node execution, CPU/GPU backend interfaces |
| `src/media` | standards-backed image/media discovery, decode, metadata, and proxies |
| `src/platform` | narrow filesystem, system, and packaging services with OS parity |
| `src/scripting` | optional Python runtime, stable proxies, package/add-on lifecycle, and API bridge |
| `modules` | optional source-built pipeline modules that register coherent capabilities |

`src/core`, `src/document`, `src/commands`, `src/ui`, and `apps/bloom` now form the first interactive
vertical slice. Other boundaries, including `src/project`, `src/host`, `src/scripting`, and
`modules`, should be created when the first proof needs their behavior rather than as empty
speculative libraries.

## Module Composition

- `apps/bloom` is the only application composition root.
- Foundation modules have explicit public surfaces and acyclic dependencies.
- Ordinary in-tree collaboration uses direct typed C++ APIs.
- Registries are reserved for extensible vocabularies such as editor types, node definitions,
  import/export adapters, and render providers.
- Optional first-party pipelines are source-built and statically linked by default.
- Host services are passed explicitly; modules do not reach through a global service locator.
- Module-provided authoring data uses stable namespaced IDs and remains preservable when its module
  is unavailable.
- Dynamic loading and a stable external ABI are future compatibility products, not accidental
  promises made by internal C++ interfaces.

The complete contract and game-engine pipeline fitness test are defined in
[`module-system.md`](module-system.md).

## Qt Boundary

- Qt 6 Widgets owns the window system, menus, standard controls, panel shell, input integration, and
  platform presentation.
- Qt types stay inside `src/ui` and `apps` unless a later accepted decision gives a narrow exception.
- The renderer does not use `QImage` as its canonical image model.
- UI signals are adapted into typed application commands.
- Every editor area hosts a replaceable panel implementation; editor type and layout are session
  state.
- The menu bar is the only fixed application surface.
- Long-running decode, evaluation, render, import, export, media scan, and proxy work never runs on
  the UI thread.

## C++ Ownership Rules

- Use C++20 as the language baseline.
- Prefer values, RAII, and explicit ownership.
- Avoid owning raw pointers.
- Use stable IDs for document references rather than pointer identity.
- Keep mutation localized behind commands and transactions.
- Use strong types for time domains and IDs where confusion would change behavior.
- Validate external and serialized data before constructing trusted document state.
- Run unit tests, warnings, sanitizers, and static analysis as the implementation grows.

## Evaluation Direction

- Start with a CPU reference evaluator for a minimal source, transform, composite, and output graph.
- Compile immutable document snapshots into runtime plans.
- Make requests explicit about time, output, resolution, quality, and color intent.
- Introduce cache keys only alongside the inputs that determine correctness.
- Validate the portable GPU direction early with a cross-platform spike, then develop GPU execution
  beside the CPU reference rather than leaving acceleration until the end.
- Keep presentation interop separate from canonical render resources.
- Discover backend capabilities at runtime and provide explicit fallbacks; do not encode a
  platform-specific API into node or project behavior.

## Standards Boundary

- Bloom's project format owns Bloom authoring data only where no suitable open standard exists.
- Color management, professional image I/O, editorial interchange, and later effect interchange use
  adopted standards behind explicit adapters.
- Imported unknown metadata is preserved where safe and practical.
- Every adapter reports unsupported, approximated, or dropped information.
- Third-party libraries do not become the document model merely because they implement an adapter.

## Current Architecture Decisions

- Replaceable editor panels are required. A custom recursive split tree is the current working
  implementation and remains gated on cross-platform UX validation; Qt docking is no longer the
  active scaffold.
- The node editor uses Bloom-owned `QGraphicsScene`/`QGraphicsView` presentation over immutable
  document snapshots; external node frameworks do not own graph, persistence, or undo semantics.
- Artist nodes lower through versioned definitions into shared checked primitives. Color, mask,
  vector, image, and future geometry values remain semantically distinct; see
  [`evaluation-primitives.md`](evaluation-primitives.md).
- `.bloom` uses a constrained ZIP container with strict deterministic JSON, explicit schema
  versions, unknown-data preservation, and atomic task-system save/open behavior; see
  [`project-format.md`](project-format.md).
- The canonical CPU image representation is premultiplied `RGBA32F` with explicit color identity;
  production color transformation remains an explicit render-service boundary.
- Dependency-management details for Qt and adopted pipeline libraries remain to be proven through
  reproducible cross-platform packaging.

The modular-monolith structure and optional pipeline model are accepted. The exact public SDK and
binary loading strategy remain deferred until an external extension workflow enters scope.

See the focused task, GPU, and platform contracts for decisions that should not be duplicated here.
