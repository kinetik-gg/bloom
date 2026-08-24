# Compositing Workspace

Status: working

Updated: 2026-08-25

Source: user-provided Bloom UI sketch dated 2026-08-25.

## Purpose

The Compositing workspace is the first product surface. It should prove that multiple editors can
observe and manipulate one composition without creating competing sources of truth.

## Sketch Inventory

The initial sketch contains:

- global File, Edit, View, Window, and Help menus
- Compositing, Editing, Grading, Scripting, and Rendering workspaces
- a compositor viewer with direct manipulation tools and playback controls
- a node editor showing source data and transforms
- a media/project browser
- a selection-driven properties editor
- a layer and animation timeline
- contextual editor headers with editor type and target selectors

Only Compositing is active in the initial scaffold. Other workspace labels communicate direction but
must not imply implemented scope.

## Shared Context Contract

All editor panels receive a read-only context containing at least:

- active project
- active composition
- current rational time
- current selection
- active tool
- command availability and dirty state
- current evaluation status and diagnostics

Panels issue typed commands for persistent changes. Selection, panel focus, hovering, layout, and
temporary drags remain session state.

## Selection Behavior

- Selecting a compatible object in viewer, timeline, graph, or media browser updates the shared
  selection.
- Properties displays the selected object's schema-backed parameters.
- A panel may eventually pin its context instead of following global selection.
- Direct manipulation in the viewer opens an interaction transaction, previews changes, and commits
  one undoable command when the interaction ends.

## Time And Direct Manipulation

Current time is exact composition-session state shared by every editor. Timeline scrubbing snaps
the playhead to the nearest exact composition frame, with halfway ties toward the greater frame,
but it never rewrites existing subframe keys.
Scrubbing requests interactive previews through a bounded newest-wins cadence; old frames cannot
publish after the desired time advances.

Viewer translation starts only through the active transform tool, a valid target, and a current
non-empty composition mapping rectangle. Batch 4 has no durable layer lock field.
Pointer motion previews a typed session override through the canonical compiler and evaluator while
leaving project revision and undo history unchanged. Release commits one constant edit or one exact-
time key insertion/update. Escape, secondary-button cancellation, pointer-capture loss, a stale
target, or a viewport/mapping change restores document truth with no command. The implementation
contract lives in
[`../architecture/animation-and-time.md`](../architecture/animation-and-time.md).

## Layer And Graph Relationship

- A composition owns one canonical graph.
- An explicit Layer Output boundary and stable Layer Stack participation make a graph result
  layer-addressable; Bloom does not infer a layer from arbitrary topology.
- Editing a layer property edits the same stable parameter used by Nodes and evaluation.
- Advanced or graph-only regions remain valid and are visibly distinguished as Structured, Custom
  Graph, or Graph-only Processing.
- Deleting or reordering a layer is a typed, localized graph mutation with exact undo rather than an
  edit to an unrelated layer model.
- Node graph scopes are filters over the same graph, never copied shadow graphs.

The working document and render contract is defined in
[`../architecture/layer-graph-model.md`](../architecture/layer-graph-model.md).

## Panel Shell

Every workspace content area is a panel. The application menu bar is the only fixed surface. Each
panel has an editor header and its editor type may be replaced without recreating or mutating project
data.

The current working implementation uses one custom recursive split tree. Every leaf is the same
editor-area type and gives the artist an editor selector plus split, close, and maximize controls.
The accepted product model is:

- an area hosts one replaceable editor implementation
- editor type and target are session/layout state
- workspaces save and restore panel arrangements
- any workspace panel may change to any registered compatible editor type
- panels may be split, resized, maximized, moved, and restored without changing document truth
- layouts do not affect project evaluation
- editors communicate through shared application services rather than direct panel-to-panel calls

Editor types come from the host-owned typed editor registry. Built-in and optional pipeline editors
follow the same context, command, lifecycle, and workspace-serialization contract. The custom
split-area tree is the working mechanism and remains gated on Linux, macOS, and Windows behavior
before it is treated as final. See
[`../architecture/workspace-layout.md`](../architecture/workspace-layout.md).

## Terminology To Resolve

- The sketch's `Source` panel behaves like a combined Media or Project browser. Its final name is
  open because `Source` may be confused with a source monitor.
- The sketch uses `Object` as node-editor context. Bloom needs a more precise term if the context is
  actually a layer, graph region, composition, or asset.
- `Compositor` currently names the viewer editor. `Viewer` may be clearer once compositing also names
  the workspace and processing domain.

## Accessibility And Density

- Dense presentation must retain readable default text and hit targets.
- Low-contrast disabled states must remain distinguishable from unavailable features.
- Keyboard focus, shortcuts, text input, and screen-reader semantics should use Qt facilities where
  practical.
- Color alone must not communicate asset, layer, proxy, cache, or error state.

## Panel-System Evaluation

The initial implementation compared the product requirements against:

1. Enhanced dock widgets with replaceable editor content.
2. A custom split-area tree with Blender-like split, merge, and editor replacement behavior.

The custom split-area tree now proves editor replacement, stable layout serialization, keyboard and
mouse focus, resizing, close, and non-destructive panel maximization. Multi-monitor floating
windows, detached areas, high-DPI transitions, and equivalent behavior on Linux, macOS, and Windows
remain acceptance gates rather than completed claims.
