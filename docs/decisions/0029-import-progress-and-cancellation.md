# ADR 0029: Import Progress And Cancellation

Status: accepted (lane decision under issue #377; the supervisor may renumber at integration)

Date: 2026-10-25

## Context

A batch import ran on the import worker with no visible progress: it succeeded or failed silently
unless the artist opened Jobs. Partial imports (#373) already committed every valid file as one
undoable `ImportAssets` transaction and reported each skipped file, but cancellation refused the
whole batch, even the files that had finished. The owner asked for a blocking, cancellable progress
modal that ends with a clear result.

## Decision

1. **One import mechanism.** Every batch import the Assets controller prepares (File > Import,
   Add > Import…, the Assets footer, file and folder drops on Assets, Timeline and Nodes) still runs
   as one `ImportAssets` preparation on the blocking-I/O task executor. The worker publishes its
   progress to the controller and to the job's `TaskProgress`; the dialog only reads it. Nothing heavy runs on
   the interface thread.
2. **Items and work.** An item is a chosen file, or one image sequence however many of its members
   were chosen. Progress is measured in work, not items: one unit per sequence member (each is read
   and hashed, the slow part) and one per other item, summed across the batch, so the bar moves
   through a single long sequence. A first **finding** phase searches chosen folders and lists each
   sequence's members without opening them, reporting what it found over a busy bar; the **reading**
   phase then probes and hashes against the known total ("Reading frame 120 of 522"). The heading
   names the item and adds "n of N" only when several were chosen. (Amended after owner review: the
   first version counted items, so one long sequence sat at 0 of 1 until it finished.) A chosen folder contributes the media files below it (images, sequence members,
   video, audio and LUTs by extension, ignoring case, sorted, hidden entries skipped); numbered
   members collapse to one sequence item before any probing. A folder with no media is one skipped
   item with a plain reason. Folders expand in the commands layer, so Python, CLI and MCP imports
   accept them too.
3. **Cancel keeps what finished.** Cancel (button, Escape or closing the dialog) asks the worker to
   stop at its next safe point: before the next item, or inside the current item's probe, hash or
   scan. The item in progress is abandoned without a record, and an interrupted read is never
   reported as a fault of the file. Every item already prepared is kept and committed as the usual
   single `Import Assets` transaction, so one Undo removes it. Rolling back was rejected: the
   finished files cost real work, and Undo already offers the rollback in one step. The application's
   own cancellation (closing the project, quitting) still discards the whole import.
4. **Clear ending.** The dialog closes on an import that added everything, and the status bar says
   `Imported N assets`. Otherwise it becomes a summary: `Import finished`, `Import cancelled` or
   `Nothing was imported`, the counts (`Imported 10 · Skipped 2 · Not imported 9`), and each skipped
   file with its plain reason (the #373 per-file failures, plus files the project already holds).
   Reader identifiers are translated into words first; for example a refused LUT reads "The LUT
   file is damaged or malformed".
5. **No flicker.** The dialog appears only after 300 ms. A shorter import never shows it, unless it
   skipped, cancelled or refused something, in which case the summary appears at once. During those
   300 ms window input (pointer, wheel, keys, shortcuts and drops) is held back, so the import blocks
   the window from its first moment without a dialog flashing up.
6. **Scripting stays non-modal.** Python, CLI and MCP construct `ImportAssets` directly and report
   each skipped file as an operation warning, unchanged.

## Consequences

- `ImportAssets` exposes `cancelled()` and `notImported()`; a cancelled batch with nothing prepared
  is refused with `Import cancelled`.
- The kit gains `KProgressBar` and the `Size::ProgressTrack` token.
- Dropping files on Timeline or Nodes imports them into the selected Assets folder; it does not add
  layers.
