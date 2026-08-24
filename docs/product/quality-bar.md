# Artist-Facing Quality Bar

Status: accepted

Updated: 2026-08-25

## Product Standard

Bloom is intended to become polished, professional motion-graphics and VFX software, not merely a
technical compositor with a desktop wrapper. Artists should be able to trust it for iterative work:
the interface stays responsive, operations are predictable and reversible, image semantics remain
correct, and the application explains anything it cannot preserve or render.

After Effects, DaVinci Resolve, and Blender are workflow references for the level of capability and
care artists already expect. Bloom may study their public behavior, terminology, and documented
workflows, but must not copy proprietary code, assets, or distinctive implementation details. A
reference application establishes an expectation to investigate, not a requirement to reproduce
every feature or interaction.

Useful public references include the [After Effects documentation][after-effects], the official
[DaVinci Resolve training material][resolve-training], and Blender's documentation for
[Editors][blender-editors] and [Areas][blender-areas].

## Artist Experience Principles

### Keep The Artist In Flow

- Ordinary input should acknowledge on the next presentable UI frame.
- Opening menus, changing panels, editing values, navigating a graph, and moving the playhead must
  not wait for media decode, rendering, cache work, or file-system traversal.
- Interactive work may use lower resolution, partial results, or progressive refinement when the
  compromise is visible. Final output must not silently use a preview compromise.
- Playback should favor stable cadence and clearly report buffering, dropped frames, proxy use, and
  cache state instead of pretending to be real-time.
- Repeated adjustments should remain interactive. Work is prioritized by current artist intent, so
  an obsolete frame or thumbnail does not delay the current request.

Exact latency, playback, and memory budgets belong to individual milestones and must be measured on
declared reference hardware. “Fast” is not an acceptance test; traces, frame timing, job latency, and
representative projects are.

### Make Heavy Work Asynchronous

Qt widgets run on the GUI thread, and Qt documents that time-consuming work should be moved to worker
threads; see [Threads and QObjects][qt-threads]. Bloom treats that as a product invariant, not an
optimization.

Potentially unbounded work must not execute on the UI thread. This includes:

- media probing, decode, encode, and image-sequence scanning
- graph evaluation, rendering, shader or color-processor compilation, and cache population
- proxy, thumbnail, waveform, and analysis generation
- project migration, large serialization, hashing, and bulk file operations
- plug-in discovery, loading, and third-party effect execution

The UI thread may validate a small command, publish already-prepared state, and present results. A
background operation needs an owner, priority, progress state, cancellation behavior, failure
diagnostics, and a rule for rejecting stale results. Cancellation is cooperative and bounded; a
feature is not complete if “Cancel” leaves the artist waiting indefinitely.

### Be Predictable Across Editors

- Viewer, timeline, node graph, media browser, and Properties observe the same project and session
  context; they do not maintain competing authoring truths.
- Selection and current time synchronize where meaningful. A pinned editor makes that divergence
  explicit.
- Editing the same parameter through a field, gizmo, layer control, or node produces the same value,
  animation, validation, and undo behavior.
- One continuous gesture creates one understandable undo step.
- Disabled, unavailable, approximated, cached, and failed states look different and explain why.
- Names, units, coordinate conventions, alpha behavior, and time domains are visible where ambiguity
  could change the result.

### Make Work Recoverable

- Every persistent artist edit flows through the command and transaction system unless an accepted
  decision records a narrow exception.
- Destructive actions state their scope and provide undo where technically honest.
- Saves are validated and atomic. Autosave and crash recovery must never overwrite the last known
  good project without an explicit recovery path.
- Missing media, effects, fonts, color configurations, and other dependencies open in a diagnosable
  degraded state whenever safe; they do not silently change the intended result.
- Errors identify the affected object and next useful action. Logs may contain technical detail, but
  the artist-facing message must stand on its own.

### Treat Image Correctness As User Experience

- Color space and display/view transforms are explicit and project-controlled.
- Viewer presentation and final output share declared color intent, with any difference visible.
- Alpha association, channel names, data/display windows, pixel aspect ratio, frame rate, and source
  interpretation are retained rather than guessed away.
- Float and high-dynamic-range values are not silently clipped by UI or intermediate storage.
- Preview degradation is labeled. Final render fails loudly when required inputs or capabilities are
  unavailable instead of substituting an unannounced approximation.
- CPU and GPU output parity is tested within a documented tolerance for each operation. “GPU
  accelerated” never means semantically different by accident.

The standards and preservation policy in [`../standards/strategy.md`](../standards/strategy.md)
governs interchange behavior.

## Flexible Workspaces And Panels

The menu bar is global. Every persistent creative work surface beneath it is an editor hosted in an
area or panel:

- Any area can switch to any registered compatible editor type.
- The same editor type may appear more than once with independent view state.
- Areas may follow shared context or visibly pin a project object or composition.
- Workspaces are named arrangements for tasks such as Compositing, Editing, Grading, Scripting, and
  Rendering; an inactive workspace label must not imply an implemented feature.
- Layout, editor type, zoom, and panel state are session or preference data and cannot affect render
  semantics.
- Maximizing, moving, replacing, and restoring an editor must preserve its meaningful context.

This adopts the artist-friendly flexibility demonstrated by Blender's editor model without requiring
Bloom to reproduce Blender's implementation. File pickers, preferences, transient popovers, and
genuine confirmations may remain dialogs when that is clearer; the rule applies to persistent work
surfaces.

## Professional Interaction Baseline

- Common actions have discoverable controls and efficient keyboard paths.
- Shortcut behavior is consistent, focus-aware, remappable in the eventual keymap system, and
  visible in menus or tooltips.
- Numeric fields support precise entry, units, keyboard adjustment, and direct manipulation without
  losing precision.
- Dense layouts remain readable at supported scale factors and do not encode status by color alone.
- Focus order, keyboard navigation, labels, contrast, and assistive semantics are part of feature
  review rather than a final cleanup pass.
- Long tasks remain inspectable in a shared job surface after the initiating panel changes or closes.
- Advanced control is progressively disclosed; a common task should not require understanding the
  entire graph or pipeline.

## Cross-Platform Contract

Linux, macOS, and Windows are product targets, not ports performed after a feature is finished.

- A feature must have equivalent user-visible semantics on all targets before it is called complete.
- Platform-specific acceleration or integration sits behind a portable capability boundary and has
  a tested fallback or an explicitly documented availability state.
- Project files and interchange results do not depend on host endianness, path separators, drive
  letters, case sensitivity, native dialog behavior, or a single vendor's GPU.
- Keyboard naming, text input, high-DPI behavior, file paths, and window management follow platform
  conventions without changing project truth.
- Continuous integration covers all three operating-system families as soon as the corresponding
  feature is in active development. GPU qualification includes more than one vendor where the
  platform permits it.

An operating-system-only implementation may be used for a prototype, but it cannot become the
semantic definition of a feature.

## Feature Definition Of Done

An artist-facing feature is not complete until the applicable questions have affirmative,
evidence-backed answers:

1. Does it fit an end-to-end artist workflow rather than expose an isolated subsystem?
2. Does it preserve the document, time, color, alpha, and media contracts?
3. Are persistent changes undoable, serializable, and deterministic where required?
4. Is expensive work asynchronous, cancellable, observable, and safe against stale completion?
5. Are preview limitations and interchange losses reported before they can mislead the artist?
6. Does it work with keyboard navigation, high-DPI displays, and non-color status cues?
7. Are behavior and failure modes tested headlessly where possible and through the UI where needed?
8. Are Linux, macOS, and Windows semantics equivalent, with platform exceptions documented?
9. Has representative project profiling shown that interaction remains responsive?
10. Is artist-facing documentation sufficient to use and troubleshoot it without reading source?

[after-effects]: https://helpx.adobe.com/after-effects/desktop.html
[resolve-training]: https://www.blackmagicdesign.com/products/davinciresolve/training
[blender-editors]: https://docs.blender.org/manual/en/latest/editors/index.html
[blender-areas]: https://docs.blender.org/manual/en/latest/interface/window_system/areas.html
[qt-threads]: https://doc.qt.io/qt-6/threads-qobject.html
