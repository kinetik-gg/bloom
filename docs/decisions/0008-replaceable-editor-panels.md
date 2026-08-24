# ADR 0008: Replaceable Editor Panels

Status: accepted

Date: 2026-08-25

## Context

Bloom's design uses a Blender-like workspace in which viewer, nodes, timeline, media, properties, and
future editors are peer surfaces. Artists should be able to adapt the workspace to a task without
the application hard-coding one permanent arrangement.

## Decision

- The menu bar is Bloom's only fixed application surface.
- Every workspace content area hosts a replaceable editor panel.
- A panel's editor type and target context are session/layout state, not project truth.
- Any compatible panel may switch to any registered editor type.
- Workspaces persist arrangements of panels and editor contexts without affecting evaluation.
- Editors communicate through application context, commands, and read models rather than direct
  panel-to-panel ownership.

The initial Qt dock-widget shell is a feasibility scaffold. Enhanced docking and a custom split-area
tree will be compared before the workspace layout mechanism is treated as final.

## Consequences

- Editor implementations need a common lifecycle, context, focus, serialization, and command
  contract.
- Layout restoration and multi-monitor behavior must be tested on Linux, macOS, and Windows.
- Maximizing, replacing, moving, or closing a panel cannot change project or render semantics.
- The application can add future editors without redesigning its main window.
