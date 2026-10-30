# ADR 0030: Layer Drops On The Timeline And Viewer

Status: accepted (lane decision under issue #380; the supervisor may renumber at integration)

Date: 2026-10-25

## Context

Dropping Assets items on the Timeline added a layer for the first dragged item only, always on top
at the composition start, and files dropped from a file manager were only imported
([ADR 0029](0029-import-progress-and-cancellation.md)). After Effects places dropped footage where
it is dropped: at a stacking position in the layer outline, at a time in the time graph, several
items at once, optionally end to end. The owner asked for the same behavior.

## Decision

1. **One operation per drop.** `AddDroppedLayers` adds one layer per item, in order, by applying the
   existing add-layer commands (`AddImageLayer`, `AddAudioLayer`, and the nested composition layer
   of `AddCompositionSource`), then `MoveLayerBefore` for the stacking position and
   `SetLayerTiming` with the Timeline's own body-move math (`proposeLayerTiming`) for the start.
   No layer is built a second way, each layer keeps the length its command gives it (layer-owned
   time, ADR 0024), and the drop is one transaction and one undo step.
2. **Placement.** The Timeline reports where the pointer is: a row of the layer stack (start at the
   playhead), a row and a frame over the lanes (the frame under the pointer, on the composition
   frame grid), or elsewhere (on top at the playhead). The Viewer always adds on top at the
   playhead; the add-layer commands center the layers. Alt at the drop sequences the items end to
   end, as After Effects' Sequence Layers; a layer that would start after the composition ends
   starts on its last frame, the same clamp a Timeline move uses.
3. **Payload.** An Assets drag also carries `application/x-bloom-layer-items`: every asset and
   composition in selection order, valid only for the session and document revision it was made
   for, like the existing single-item payloads, which the Nodes canvas keeps using unchanged.
4. **Files import, then become layers.** A file drop on the Timeline or the Viewer runs the
   ordinary import (ADR 0029: worker preparation, progress modal, cancel keeps what finished) with a
   follow-up. Once the import transaction commits, the follow-up adds layers for every committed
   item, including files the project already held, at the placement captured at the drop, as a
   second transaction. The import report carries what it did ("Added 2 layers") into the status
   line or the summary. Undo therefore removes the layers first and the import second. Adding the
   layers inside the import transaction was rejected: the import is prepared on a worker against a
   snapshot, while layer placement depends on the composition the artist is looking at when the
   import finishes.
5. **Selection.** After a drop (and after a file drop's follow-up) the new layers are selected,
   the uppermost primary, as in After Effects. This is a session selection change, not part of
   the transaction.
6. **Indicators.** The layer stack reuses the reorder insertion line; the lanes paint the same
   line and an Accent time marker. Both are session-only presentation.

## Consequences

- ADR 0029's statement that a Timeline file drop adds no layer is superseded for the Timeline and
  the Viewer; Nodes drops still only import.
- `AssetController::importFiles` accepts an optional follow-up, and `ImportReport` lists the
  committed assets and the follow-up's sentence.
- Dropping the active composition onto itself, fonts and LUTs is refused before the drop is
  accepted; a deeper nesting cycle is refused by the nested composition command, with no change.
- `AddDroppedLayers`, the nested composition layer of `AddCompositionSource` and the Timeline's
  body-move math (`proposeLayerTiming`) are Qt-free commands in `src/commands`
  (`layer_drop_operations.hpp`, `node_editor_operations.hpp`, `layer_timing_gesture.hpp`). Scripts
  and MCP agents run the same drop as `bloom.layer.add-from-assets`, naming the placement layer by
  its layer id instead of its stack slot.
