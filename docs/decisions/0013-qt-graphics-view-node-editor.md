# ADR 0013: Qt Graphics View For The Node Editor

Status: accepted

Date: 2026-08-25

## Context

Bloom needs a professional node editor over the canonical composition graph. The editor must support
custom node geometry, typed ports, large canvases, precise selection and navigation, multiple graph
scopes, plugin-contributed node descriptions, and Bloom's command and undo semantics.

A generic node-editor framework can accelerate a prototype, but many frameworks also own a graph
model, propagation, persistence, and undo stack. Those responsibilities already belong to Bloom's
document, command, project, and runtime modules and must remain identical across Nodes, Timeline,
Properties, Python, and headless use.

## Decision

- Build the first node-editor presentation on Qt 6 Widgets' `QGraphicsScene` and `QGraphicsView`.
- Keep node, port, edge, selection, and diagnostic graphics as Bloom-owned UI types under `src/ui`.
- Project immutable document snapshots into the scene. The scene never becomes project truth and
  never serializes graph semantics.
- Route graph edits through the same typed command and transaction gateway as every other editor.
  `QGraphicsItem` movement and gestures hold only transient interaction state until committed.
- Keep graph layout, zoom, framing, collapsed state, and visual grouping in session/editor state
  unless an explicit document feature requires shared durable layout.
- Do not adopt QtNodes or another generic node framework as a foundational dependency for the first
  proof. It may be benchmarked later behind the same Bloom-owned presentation boundary.
- Use the normal cross-platform Qt paint path initially. Do not force an OpenGL viewport merely to
  label the canvas GPU accelerated; profile representative graphs before choosing a viewport mode.
- Keep compositing GPU resources and APIs out of the node-editor presentation layer.

## Quality Gates

Before the node editor is considered production-ready, representative fixtures must measure pan,
zoom, selection, edge redraw, node movement, and snapshot refresh behavior. Tests must cover stable
ID mapping, command emission, stale-item removal, multi-selection, keyboard focus, high-DPI
rendering, and equivalent behavior on Linux, macOS, and Windows.

The implementation must support culling and level-of-detail presentation without hiding selected,
failed, or otherwise semantically important items. Performance tuning remains evidence-driven using
the indexing and viewport update modes provided by Qt.

## Consequences

- Bloom owns more presentation code, but does not reconcile a third-party graph or undo model with
  its canonical graph.
- UI behavior can evolve with Bloom's layer/graph projection and extension contracts without making
  an external library's serialization or identifiers part of the project format.
- Qt's existing scene, item, focus, selection, transform, and event infrastructure remains available
  without adding another shipping dependency.
- A later renderer or framework replacement stays localized because the document-to-view adapter and
  command boundary are Bloom-owned.

Primary references:

- [Qt Graphics View Framework](https://doc.qt.io/qt-6/graphicsview.html)
- [`QGraphicsScene`](https://doc.qt.io/qt-6/qgraphicsscene.html)
- [`QGraphicsView`](https://doc.qt.io/qt-6/qgraphicsview.html)
- [QtNodes evaluation candidate](https://github.com/paceholder/nodeeditor)
