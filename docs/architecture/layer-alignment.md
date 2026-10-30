# Layer Alignment

Status: canonical

Updated: 2026-10-30

Align and distribute move layers so their evaluated visual bounds line up. The decision and its
rationale are [ADR 0031](../decisions/0031-contextual-tool-bar.md); this document is the
implementation contract.

## Pieces

| Piece | Module | Role |
| --- | --- | --- |
| `commands::AlignLayers`, `commands::planLayerAlignment` | `src/commands` | The one command, and the pure plan it applies. Qt-free and evaluation-free. |
| `runtime::queryLayerBounds`, `runtime::layerOutputBounds` | `src/runtime` | Headless bounds: compile one snapshot and evaluate one frame on the CPU reference path. |
| `host::layerAlignmentGeometry`, `host::measureLayerAlignmentGeometry` | `src/host` | Turn evaluated bounds into the command's `LayerAlignmentGeometry`; the headless measurement. |
| `bloom.layer.align-layers`, `bloom.layer.distribute-layers` | `src/scripting` | Python, MCP and JSON operations that measure headlessly and queue the command. |

The command takes, for one composition and one composition time, the layers to move, the
arrangement, the target, an optional key layer, and a `LayerAlignmentGeometry` per measured layer:
its output bounds, the linear part of its parent's world transform, and its Position sampled at its
own key time. It never samples a curve or evaluates a frame itself, so it stays inside the commands
module's dependency boundary.

## Bounds

A layer's bounds are its Layer Output boundary's `EvaluatedOperationBounds::output` at the time: the
axis-aligned rectangle around the layer's transformed content polygon in composition pixel-edge
coordinates, at full composition resolution, not clipped by the frame. A layer outside its range,
disabled, or with empty content has no bounds and is skipped.

`runtime::layerOutputBounds(plan, bounds)` is the one filter (a Layer Output operation, a valid
layer id, non-empty output) over an evaluated frame's per-operation bounds. Both sources go through
the same evaluator semantics:

- **Interactive.** The Viewer has the bounds of the frame it was delivered,
  `CompositionPreviewController::currentLayerBounds()` (translated to the live layer identities).
  This is metadata on the delivered frame: no evaluation on the UI thread. `ui::LayerAlignmentActions`
  (the toolbar's buttons and Composition > Align) uses that frame only while its source revision,
  composition and time are the session's current ones, and is disabled with a reason otherwise; it
  orders the selected layers top of the stack first and takes the primary selected layer as the key.
- **Headless.** `runtime::queryLayerBounds` compiles the snapshot and evaluates the frame with the
  CPU reference evaluator (full resolution, look nodes included, media revalidated), then applies
  the same filter. It is heavy and runs on the calling client thread (script, MCP or CLI), never
  the UI thread. It takes a cancellation token for task callers and a bounded pixel budget
  (`kDefaultLayerBoundsPixelBudget`).

`bloom.ui.layer-alignment-parity` pins that the delivered CPU preview frame and the headless query
give the same bounds, geometry and planned move for the same snapshot and time;
`bloom.scripting.layer-alignment` pins the same against the GPU scene preparation's bounds, which
are what a GPU-resident delivered frame carries.

## Geometry

`host::layerAlignmentGeometry(composition, time, bounds)` builds one geometry per distinct layer:

- **Position** at the layer's key time `(t - start) * speed` (`commands::layerKeyTimeOfCompositionTime`,
  ADR 0024): the constant, or the curve sampled with `runtime::sampleAnimationCurve`.
- **Parent transform.** The evaluated polygon is `world * local corners`, so the layer's world
  linear map is read from the polygon edges over the local size. With the layer's own sampled scale
  and rotation (`LayerMatrix::authored`'s linear part, quarter turns exact) the parent's linear map
  is `world * own^-1`. When the layer's own transform is not sampleable (driven), the parent's own
  evaluated polygon gives the parent's world map directly. Otherwise the geometry is singular and
  the command skips the layer.

## The Command

`planLayerAlignment` is the whole decision without a document change; `AlignLayers::apply` applies
exactly that plan.

1. **Validation.** The composition must match, at least one layer must be named, every layer and the
   key must belong to the composition, every geometry rectangle must be finite and non-empty and
   named once, Key Object needs a key with bounds, and distribution needs at least
   `kMinimumDistributedLayers` (3) layers including the key. A refusal names the reason.
2. **Target.** Composition is `(0, 0, width, height)`. Selection is the union of the bounds of every
   requested layer that has bounds (skipped ones included: they are part of what the artist
   selected). Key Object is the key's bounds; the key is never moved even when it is also requested.
3. **Deltas.** An alignment gives each layer the one-axis delta from its edge or center to the
   target's. A distribution sorts the layers (and the key) by center, request order breaking ties,
   fixes the outermost two (Composition first moves them so their outer edges meet the frame's
   edges) and, for Key Object, the key, then spaces the centers between each pair of fixed layers
   evenly.
4. **Skips.** A layer is skipped, with a typed `LayerAlignmentSkipReason` and a message, when it is
   locked (its boundary or its Position parameter), has no bounds, has no Vec2 Position, has a
   driven Position (a value-graph driver), has an animated Position with no key time or no sampled
   value, or has a parent transform without a finite inverse. Skips are reported as warnings and
   `skippedLayer` outputs. If every requested layer other than the key is skipped, the command is
   rejected with every reason and nothing changes.
5. **Parenting.** A moving ancestor carries its descendants rigidly by its whole delta, so a layer's
   own Position takes only what its nearest moving ancestor (through any number of non-moving
   ones) does not already supply. The remainder is converted into parent space with
   `parentLinear^-1`; a translation delta does not depend on any anchor.
6. **Writing.** A constant Position is set with `SetParameterSource`. An animated Position gets
   `SetKeyframeAtTimeForParameter` at the layer's key time with the sampled value plus the delta; a
   key already at that time is updated and keeps its id; every other key is untouched. A layer
   already within `1e-9` pixels of its target is not written.
7. **One step.** The operation is one transaction and one undo entry. It edits only parameters and
   curves of one composition (`parameterEditScope`), so the command stack proves its time
   footprint by diffing. Moved layers are `movedLayer` outputs.

No document schema changes and no pixel operation is added, so save/reopen and the GPU coverage
contract are unaffected.

## Scripting

Both operations measure through `OperationContext::layerGeometry` when they are created: Python,
MCP and `bloom-cli` contexts carry a provider that runs `host::measureLayerAlignmentGeometry` on the
session's render-source snapshot (with its color intent and asset base directory). Python releases
the GIL while it measures. A context without a provider creates the operation without geometry,
which the command then rejects. Their `layers` argument defaults to the selected layers
(`ContextSource::Layers`, published as `"layers"`), `time` to the playhead and `composition` to the
current composition. The geometry is the snapshot the transaction starts from: layers an earlier
operation of the same transaction adds or moves are measured as they were before it.
