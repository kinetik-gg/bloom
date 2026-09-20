# Project Lifecycle And New Composition

Status: working

Updated: 2026-09-20

This document owns the user-visible lifecycle of a new Bloom project and the New Composition
dialog. It records behavior implemented in `ProjectHost`, `CompositionSession`, the Assets editor,
and the Composition menu. Architecture detail lives in
[`../architecture/project-session.md`](../architecture/project-session.md).

## Blank Startup

Opening Bloom starts on a fresh, untouched project. That project is **blank**: it owns its identity,
name, and colour settings, and **no composition at all**. A composition is never created implicitly,
and no placeholder composition is created and deleted behind the artist's back.

A project with no composition is a valid new document. It is decoded, editable, and clean; it has no
active composition, no selection, and zero command history, so an untouched blank project closes
without an unsaved-change prompt and saves normally. File > New follows the same blank default.
Headless and fixture callers that genuinely need a composition ask for one explicitly; production
never auto-seeds one.

## Empty State

Because a blank project exposes no active composition, every replaceable editor shows an honest
empty state rather than stale or invalid controls:

- The viewer canvas shows its quiet empty-state invitation.
- The node canvas is empty.
- The Assets panel keeps its `Compositions` root with no children and still offers New Composition
  and Import.
- The timeline shows no rows, and the Properties panel disables composition-dependent controls.
- The composition viewer and timeline composition selectors show that nothing is active.

The application still quits and shuts down cleanly with a task-free blank project.

## Composition Commands

- **New Composition** opens the composition dialog and, on success, adds one composition through the
  ordinary command path and makes it active.
- **Duplicate** and **Rename** are available only while a composition is active.
- **Delete** removes the active composition through one undoable command. The final remaining
  composition stays protected; Delete is not offered when it would empty the project.
- Deleting an active composition selects the lowest remaining composition; undo and redo behave as
  for any other command.

All of these are ordinary commands on the project's single command stack, so undo, autosave,
scripting, validation, and invalidation observe the same mutation.

## New Composition Duration

The dialog labels the row **Duration** and pairs a numeric field with a unit selector to its right:

- **Frames** is the default unit. The default value is ten seconds expressed in frames at the dialog's
  frame rate (ten seconds at 24 fps is 240 frames), never a literal ten frames.
- **Seconds** is the alternate unit. Frames are whole numbers; seconds may be fractional.

Rules:

- Changing the frame rate keeps the entered number: frames stay frames, seconds stay seconds. The
  resulting duration follows naturally (for example 240 frames at 30 fps is eight seconds).
- Switching units converts the displayed number so the duration is unchanged, clamped to the frame
  grid at the frame rate. Switching Frames -> Seconds -> Frames restores the same frame count.
- The stored value is an exact rational time: `frames / frameRate` for frames, an exact
  microsecond-based rational for seconds.
- A duration that is zero, negative, or outside the supported range is not accepted; the numeric
  field's range keeps such values out and the dialog refuses to commit one.
- When a composition is already active, the dialog inherits that composition's frame rate and
  duration as its defaults.

## Related Contracts

- [`foundation.md`](foundation.md)
- [`../ux/interaction-model.md`](../ux/interaction-model.md)
- [`../ux/compositing-workspace.md`](../ux/compositing-workspace.md)
- [`../architecture/project-session.md`](../architecture/project-session.md)
