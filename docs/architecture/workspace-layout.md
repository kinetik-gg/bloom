# Workspace Layout

Status: working

Updated: 2026-09-17

## Purpose

Bloom's workspace layout is session state that arranges replaceable editor areas. It must remain
independent of project truth, command history, evaluation, and rendered output.

The current implementation is a custom recursive split tree built with Qt Widgets. It replaces the
initial mixed central-splitter and dock-widget feasibility shell and is the working mechanism for
the first Compositing workspace proof.

## Runtime Structure

- `WorkspaceHost` owns one recursive split tree.
- A normal split is a non-collapsible `QSplitter` with an orientation, at least two children, and
  proportional weights. The authored default uses one four-child horizontal top row and one
  two-child vertical workspace-row split.
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
- reset the Compositing workspace to the owner's five-area layout
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

The first-run and `Window → Reset Workspace` arrangement is authored in `WorkspaceHost` as
per-mille splitter weights, applied after the complete tree receives its window extent:

| Split | Children, left-to-right or top-to-bottom | Weights |
| --- | --- | --- |
| Top row | Assets, Viewer, Nodes, Properties | 160 / 310 / 320 / 190 (16% / 31% / 32% / 19%) |
| Workspace rows | Top row, Timeline | 680 / 320 (68% / 32%) |

The Timeline layer-table/lanes divider defaults to 37% of the Timeline width. Its persisted pixel
width remains authoritative for an existing session; Reset Workspace clears that width and writes
the newly resolved 37% default together with the workspace tree. Viewer starts at RGBA/Fit, Nodes
starts fitted, and Properties shows Composition when there is no selection. These are editor-owned
presentation defaults and do not enter document state.

Schema version 2 stores this five-area default as a direct four-child top row over Timeline. The
sidebar no longer spans the workspace height. Assets is pinned into the default and migrated
layout, while editor areas remain replaceable during use. The menu bar remains the only fixed
application surface.

The application first validates and restores a legacy version-1 layout, then replaces its panel
arrangement with this complete default. Window geometry is retained. This intentional reset prevents
old saved arrangements from omitting Assets or giving Timeline the sidebar's width. Subsequent saves
write schema 2. The generic WorkspaceHost still reads valid version-1 trees for its callers; the
application owns the Compositing migration policy. Invalid layouts keep the safe default, and future
versions retain the existing do-not-overwrite guarantee.

Split fractions are applied once the complete widget tree receives its window extent. Context-bound
callbacks are retired if that tree is replaced before showing, so restoration cannot be overwritten
by pending default-layout work. Minimum widths continue to apply normally. The timeline left column
uses the sum of its token-defined columns; metric audits verify full menus at 1600 and 1920 pixels.
The main-window migration test starts with a valid one-area version-1 layout without Assets and
asserts a five-area schema-2 result containing Assets.

## Acceptance And Gating

The split tree remains a working product mechanism until its focus, scaling, persistence,
maximization, and recovery behavior is exercised on Linux, macOS, and Windows. The implementation
must retain equivalent semantics on each platform even if menu presentation or window chrome is
platform-owned.

The following remain deferred:

- drag-from-corner splitting and drag-to-merge gestures
- tabs, floating areas, and detached windows
- multiple functional workspaces and user-named presets
- per-editor internal session-state migration
- keymap customization
- layout undo history

These deferred features must extend the same session-state and editor-registry contracts rather
than creating a second workspace mechanism.

## Script editor

`bloom.script` is a replaceable registered editor with kit input, history, output and shared editor
header/footer controls. It is offered by the panel switcher without changing the five-area default
layout. Python-enabled builds attach it to the live host; other builds show an explicit unavailable
state. See [Scripting Bloom with Python](../user-guide/python.md) for execution and undo behavior.
