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

## Layer And Graph Relationship

- A composition owns one canonical graph.
- A compatible graph region may be represented as a layer.
- Editing a layer property edits the corresponding canonical graph parameter.
- Advanced or graph-only regions remain valid but must be visibly distinguished in the layer view.
- Deleting or reordering a layer compiles into explicit graph mutations rather than silently editing
  an unrelated layer model.

The exact graph-region representation remains an implementation decision for the vertical proof.

## Panel Shell

Every workspace content area is a panel. The application menu bar is the only fixed surface. Each
panel has an editor header and its editor type may be replaced without recreating or mutating project
data.

The current scaffold uses Qt dock widgets as a low-cost proof and gives each editor a type selector.
The accepted product model is:

- a dock hosts one replaceable `EditorPanel`
- editor type and target are session/layout state
- workspaces save and restore panel arrangements
- any workspace panel may change to any registered compatible editor type
- panels may be split, resized, maximized, moved, and restored without changing document truth
- layouts do not affect project evaluation
- editors communicate through shared application services rather than direct panel-to-panel calls

Editor types come from the host-owned typed editor registry. Built-in and optional pipeline editors
follow the same context, command, lifecycle, and workspace-serialization contract.

The precise layout mechanism—enhanced Qt docking or a custom split-area tree—will be chosen by a UX
prototype. Replaceability is not optional regardless of mechanism.

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

The implementation spike should compare:

1. Enhanced dock widgets with replaceable editor content.
2. A custom split-area tree with Blender-like split, merge, and editor replacement behavior.

The selected mechanism must support stable layout serialization, multi-monitor floating windows,
high-DPI transitions, keyboard focus, panel maximization, and equivalent behavior on Linux, macOS,
and Windows.
