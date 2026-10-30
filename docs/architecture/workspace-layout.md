# Workspace Layout

Status: working

Updated: 2026-10-30

## Purpose

Bloom's workspace layout is session state that arranges replaceable editor areas. It must remain
independent of project truth, command history, evaluation, and rendered output.

The current implementation is a custom recursive split tree built with Qt Widgets. It replaces the
initial mixed central-splitter and dock-widget feasibility shell and is the working mechanism for
the first Compositing workspace proof.

## Runtime Structure

- `WorkspaceHost` owns one recursive split tree.
- A normal split is a non-collapsible `QSplitter` with an orientation, at least two children, and
  proportional weights. The authored default nests a full-height right column (Assets over
  Properties) beside a left region whose top row is Viewer | Nodes and whose bottom is the Timeline.
- A leaf is an `EditorArea` with a stable area ID and one selected stable editor type ID.
- `EditorArea` instantiates only its selected editor. Switching type destroys the old editor widget
  and creates the selected editor without mutating project data.
- The application-lifetime `EditorRegistry` owns editor descriptors and factories. Editor type
  identity never depends on display text, picker order, widget position, or translated labels.
- The built-in `bloom.assets` editor is the Assets composition browser. Its Folder icon and
  display name are presentation metadata; composition identity remains the document's stable
  `CompositionId`.
- Mouse and keyboard focus establish the active area. Commands resolve the active area at
  invocation time rather than capturing a persistent widget position.

The widget tree is the current UI-session representation. It must not acquire document objects,
render resources, task ownership, or project mutation responsibilities. Extract a Qt-free layout
model only when another consumer or independently testable session behavior justifies that
boundary.

## Supported Operations

The first slice supports:

- replace the editor type in any area
- split the active area left/right or top/bottom
- close an area while preserving at least one leaf
- maximize and restore an area without rewriting the underlying tree
- reset the Compositing workspace to the Default preset
- save and restore topology, proportions, area IDs, active area, and editor type IDs

Split, close, maximize, restore, and reset are available through visible Window-menu actions. Area
headers may expose the same actions. No dedicated default shortcut is assigned yet; shortcut and
remapping policy needs a cross-platform keymap design.

While one area is maximized, off-path branches are hidden but remain in the tree. Split and close
are disabled until the layout is restored. Maximizing must not change serialized topology,
proportions, editor choices, or project state.

## Persistence Contract

The Compositing layout is stored through `QSettings` as versioned compact JSON with:

- format identifier `bloom.workspace-layout`
- integer schema version
- recursive area and split nodes
- stable area and editor type IDs
- split orientation and normalized proportional weights
- active-area identity

This data is user/session preference state, not part of a `.bloom` project.

Restoration validates format, schema, depth, node count, split child counts, identities,
orientations, and weights before replacing the live layout. Invalid data leaves the safe default
layout intact. A future unsupported schema is not overwritten automatically when Bloom exits.

An unavailable editor type is localized to its area. Bloom preserves the missing stable editor ID,
shows an unavailable-editor placeholder, and lets the artist select a registered editor. Other
areas continue to restore normally.

Editor-internal session state, pinned context, zoom, scroll positions, detached windows, and
multi-monitor geometry are not yet part of schema version 2.

## Authored default and Version 2 migration

The first-run and `Window → Reset Workspace` arrangement is the built-in **Default** workspace
preset (owner, 2026-10-31), authored in `WorkspacePresets` as normalized split weights and applied
through `WorkspaceHost::applyArrangement()`:

| Split | Children, left-to-right or top-to-bottom | Weights |
| --- | --- | --- |
| Workspace columns | Left region, Properties | 80% / 20% |
| Left region | Assets/Viewer row, Timeline | 66% / 34% |
| Assets/Viewer row | Assets, Viewer | 25% / 75% |

The Timeline layer-table/lanes divider defaults to 37% of the Timeline width. Its persisted pixel
width remains authoritative for an existing session; Reset Workspace clears that width and writes
the newly resolved 37% default together with the workspace tree. Viewer starts at RGBA/Fit and
Properties shows Composition when there is no selection. These are editor-owned presentation
defaults and do not enter document state. The Workspace field then reads "Default". Nodes is no
longer in the default; the Node preset has it. The older five-area arrangement (Viewer | Nodes
beside Assets over Properties) is gone; a saved workspace of an existing session is restored as it
is and is never replaced by a preset.

Schema version 2 stores the arrangement as a tree of areas and splits. The menu bar and the
contextual tool bar under it remain the only fixed application surfaces
([ADR 0031](../decisions/0031-contextual-tool-bar.md)).

The application first validates and restores a legacy version-1 layout, then replaces its panel
arrangement with the Default preset. Window geometry is retained. This intentional reset prevents
old saved arrangements from omitting Assets or giving Timeline the sidebar's width. Subsequent saves
write schema 2. The generic WorkspaceHost still reads valid version-1 trees for its callers; the
application owns the Compositing migration policy. Invalid layouts keep the safe default, and future
versions retain the existing do-not-overwrite guarantee.

Split fractions are applied once the complete widget tree receives its window extent. Context-bound
callbacks are retired if that tree is replaced before showing, so restoration cannot be overwritten
by pending default-layout work. Minimum widths continue to apply normally. The timeline left column
uses the sum of its token-defined columns; metric audits verify full menus at 1600 and 1920 pixels.
The main-window migration test starts with a valid one-area version-1 layout without Assets and
asserts the Default preset's four areas, including Assets, with the preset named Default.

## Workspace presets

A preset is a named arrangement, stored as the same versioned layout state `saveLayoutState()`
writes (topology, proportions, stable editor type IDs); it never holds project data or an editor's
own state. `WorkspacePresets` (`src/ui`) owns the list: built-in Default, Node, Export and Animation
are authored in code, the artist's own are stored in application preferences under
`workspace/presets/user` (a JSON array of `{name, layout}`), and the active preset's name under
`workspace/presets/active`. Names are unique across both lists, case-insensitively, and a built-in's
name cannot be reused. The context bar's Workspace field reads this one list; there is no menu for it.

`WorkspaceHost::applyArrangement()` applies a preset without rebuilding what it keeps: each area of
the new tree is taken from the live tree when one hosts the same editor type (so zoom, scroll and
selection survive), created otherwise, and areas with no place close. The state is validated in a
dry pass before any widget moves, so an invalid state changes nothing. It runs through the same
native-surface retirement gate as every other mutation. Reset Workspace applies the Default preset, so
the field reads Default.

`WorkspaceHost::layoutChanged()` fires for splits, closes, divider drags, editor swaps and restored
layouts. After a preset is chosen, the picker compares the live arrangement (topology, editor types,
divider weights within a tolerance of six percent) with the preset and shows "<name> (edited)" while
they differ.

## Reveal policy

`WorkspaceHost::revealEditor(id)` shows an editor and makes its area active and focused: the area
that already hosts it, else a new area split below the Timeline's. Per-editor policy overrides the
second step. `bloom.export` is never split off: with an Export area open it is only activated; else
an open Timeline's area switches to Export in place; else a new area takes the bottom of the whole
workspace.

## Acceptance And Gating

The split tree remains a working product mechanism until its focus, scaling, persistence,
maximization, and recovery behavior is exercised on Linux, macOS, and Windows. The implementation
must retain equivalent semantics on each platform even if menu presentation or window chrome is
platform-owned.

The following remain deferred:

- drag-from-corner splitting and drag-to-merge gestures
- tabs, floating areas, and detached windows
- per-workspace content beyond the arrangement (a preset stores panel arrangement only)
- per-editor internal session-state migration
- keymap customization
- layout undo history

These deferred features must extend the same session-state and editor-registry contracts rather
than creating a second workspace mechanism.

## Script editor

`bloom.script` is a replaceable registered editor with kit input, history, output and shared editor
header/footer controls. It is offered by the panel switcher without changing the default
layout. Python-enabled builds attach it to the live host; other builds show an explicit unavailable
state. See [Scripting Bloom with Python](../user-guide/python.md) for execution and undo behavior.

## MCP Server editor

`bloom.mcp-server` (display name **MCP Server**, ADR 0034) is a replaceable registered editor for the
in-app MCP server. It holds no server state: every MCP Server editor and the status bar read one
application-lifetime `McpServerAdapter`, which owns the `mcp::McpServerService`, drains it on the UI
thread and emits one change signal per published snapshot. Closing, duplicating or swapping the
editor leaves the server running. The editor issues the service's commands (Start, Stop, Port,
Start when Bloom opens, Reveal, Copy, Regenerate, Copy client config) and shows each typed refusal's
reason verbatim under the control it belongs to; port, auto-start and the token are preferences,
never project state, and nothing the editor does enters the undo history. See
[Using Bloom from an agent](../user-guide/mcp.md).
