# ADR 0031: Contextual Tool Bar, One Active Tool, And Layer Alignment

Status: accepted (owner decisions of 2026-10-29 for the UI-kit polish release; the supervisor may
renumber at integration). Supersedes the "menu bar is the only fixed application surface" clause of
[ADR 0008](0008-replaceable-editor-panels.md).

Date: 2026-10-30

## Context

[ADR 0008](0008-replaceable-editor-panels.md) made the menu bar Bloom's only fixed surface and
every workspace area a replaceable panel. Tools then lived in a column inside each Viewer, so each
Viewer had its own active tool, tool options sat on the canvas, and there was nowhere to put
selection actions such as aligning layers. After Effects and Illustrator put the tools, the active
tool's options and the selection's actions in one bar under the menu bar, with one active tool for
the whole application. The owner asked for the same, and for align and distribute.

## Decision

1. **Two fixed surfaces.** The menu bar and the contextual bar directly under it are Bloom's fixed
   application surfaces. Every workspace content area stays a replaceable editor panel; nothing
   else becomes fixed. The bar can be hidden with Window > Toolbar. Its visibility is
   session/UI state persisted like other workspace UI state, never project truth.
2. **One app-wide active tool (the After Effects model).** One application-level, typed owner holds
   the active tool, passed explicitly to the Viewers that use it (no service locator or event bus).
   The tool acts in whichever Viewer receives the gesture. Temporary overrides (Space for Hand and
   similar) stay per gesture. The existing rules stay: bare letters choose tools, creation tools are
   one-shot and return to Select, Escape confirms, a zero-size press keeps the tool, and choosing a
   tool alone never authors anything. The per-Viewer tool column goes away.
3. **Three regions.** Left: the tools. Middle: options for the active tool, backed only by real
   session or document state. Right: actions on the selection (align and distribute, with the
   Align To target). Within a tool the slots are stable: an action that does not apply is
   disabled with a reason, never hidden. Narrow windows collapse groups into an overflow menu.
4. **The bar only issues commands.** It reads session and document state and issues the same typed
   commands every other surface uses. It owns no project truth, and nothing it does bypasses the
   command stack or undo.
5. **Alignment semantics.** One command, `commands::AlignLayers`, aligns (left, horizontal center,
   right, top, vertical middle, bottom) or distributes (horizontal or vertical centers) a set of
   layers against a target:
   - **Bounds** are each layer's evaluated visual bounds at the time: the axis-aligned
     composition-space rectangle around its transformed content (`EvaluatedOperationBounds::output`,
     not clipped by the frame).
   - **Targets.** Composition is the composition frame. Selection is the union of the requested
     layers' bounds. Key Object is one layer's bounds, Illustrator's key object; the key never moves.
   - **The move** is a composition-space delta converted into the layer's parent space through the
     parent's evaluated world transform (rotation, scale and shear; a translation does not depend on
     any anchor) and added to Position. A layer carried by a moving ancestor moves only by the rest.
   - **Animation.** An animated Position gets a key at the time, in the layer's own key time
     (ADR 0024); its other keys stay. A constant Position is set.
   - **Distribution** keeps the outermost centers fixed (Selection), places the outermost layers'
     outer edges on the frame's edges (Composition), or also keeps the key fixed and spaces the
     layers between fixed layers evenly (Key Object). It needs three or more layers.
   - **Skips.** Locked layers, layers whose Position is driven, layers with no visible bounds at the
     time and layers under a singular parent are skipped and reported with a reason, never
     silently. If every layer is skipped the command is rejected and nothing changes.
   - **One step.** The whole operation is one transaction and one undo entry.
6. **Where the bounds come from.** The command never evaluates. The Viewer passes the bounds of the
   frame it was delivered (`CompositionPreviewController::currentLayerBounds`, metadata only, no
   UI-thread evaluation). Scripting, MCP and `bloom-cli` measure with `runtime::queryLayerBounds`,
   the CPU reference evaluation of one frame, on the client thread. Both turn the bounds into the
   command's geometry with `host::layerAlignmentGeometry`, so the same snapshot and time give the
   same move (a parity test pins this, and the headless query also matches the GPU scene
   preparation's bounds).

## Consequences

- The workspace keeps one fixed strip of chrome more; it is not a panel and cannot be replaced,
  docked or moved, only hidden.
- Tool state moves out of `ViewerEditor` into the application-level owner, `ui::ToolController`,
  which the window's `CompositionSession` owns and hands to every Viewer and the bar it is given to
  (the same explicit reference every editor already receives); Viewers read it. The bar is a row of
  the window's central column rather than a `QToolBar`, so the native macOS menu bar is untouched.
- Align and distribute are available to the bar, the menus and scripting
  (`bloom.layer.align-layers`, `bloom.layer.distribute-layers`) with one meaning. They add no
  document schema and no pixel operation, so the GPU coverage contract is unchanged.
- A scripted alignment measures the snapshot its transaction starts from; layers that an earlier
  operation in the same transaction adds or moves are measured as they were before it.
