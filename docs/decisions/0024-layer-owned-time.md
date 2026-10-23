# ADR 0024: Layer-Owned Time

Status: accepted (owner approval 2026-10-23)

Date: 2026-10-23

Owner decision: 2026-10-23, issue #353. The time mapping is layer-owned; everything feeding a Layer
Output evaluates at the layer-mapped time. This record supersedes the "absolute composition time"
rule stated in [`animation-and-time.md`](../architecture/animation-and-time.md) and
[`layer-graph-model.md`](../architecture/layer-graph-model.md).

## Context

A `LayerOutputBoundary` records only an activity window (`inPoint`, `outPoint`). Media, nested
compositions, animation curves and value nodes are sampled at absolute composition time, so a layer
bar cannot move its content:

- `SetLayerRange` changes only when a layer is visible. Moving a 24-frame sequence bar to frame 50
  shows sequence frames 50-74, which is the held last frame for the whole bar.
- Image and video sources carry a constant Integer `startFrame` and `loopMode`; composition sources
  carry animatable `timeOffset`/`timeScale` and a constant `loopMode`. No Timeline gesture edits any
  of them, and they live on source nodes that a Merge can also consume without any layer.
- `AddImageLayer` and `AddAudioLayer` never set a range. The full-duration sentinel turns a
  24-frame sequence in a 100-frame composition into a 100-frame bar with 76 held frames.
- Keyframes stay where they are when a layer moves.
- Document validation requires `0 <= in < out <= duration`, so a clip cannot start before frame 0,
  and `SetCompositionDuration` is refused by commit validation whenever an explicit layer range
  ends after the new duration.

Bloom already has one exact mapping from a parent time to a child time: `mapCompositionTime`
(`src/runtime/composition_time.cpp`), with checked rational arithmetic, nanosecond fallback, and
Hold/Loop/PingPong wrapping. The CPU evaluator, GPU scene preparation (`gpu_scene_nested.hpp`),
storage preflight and audio clip mapping (`mapAudioClipTime`) all use it. The operation cache already
keys time-dependent work by exact time and omits time for invariant work.

## Decision

### 1. Two clocks per layer

A layer converts its placement time `t` (the time of the scope that contains the Layer Output;
composition time for a top-level layer) into two clocks:

```text
layer time    tau = (t - startTime) * speed
content time  c   = outOfExtent(remap ? exact(timeRemap sampled at tau) : tau)
```

- The Layer Output's own parameters (transform, opacity, driven blend mode, and the Time Remap curve
  itself) are sampled at `tau`.
- Everything feeding the Layer Output's image and audio inputs -- sources, effects, Merges, nested
  layers, composition sources, value nodes driving those operations -- evaluates at `c`.
- The activity window `[inPoint, outPoint)` stays in placement time and is tested first, exactly as
  today.

Transform animation therefore keeps playing through a Time Remap freeze, and the remap curve is
sampled on a clock that does not depend on its own output. Moving `startTime` moves every keyframe of
the layer and of its upstream implicitly; no key is rewritten by a timing gesture.

### 2. Durable layer timing

`LayerOutputBoundary` gains, all optional with identity defaults:

| Member | Type | Default | Rule |
| --- | --- | --- | --- |
| `startTime` | exact `RationalTime` | `0` | Placement time of layer time zero. Any representable value, including negative and beyond duration |
| `speed` | exact positive rational | `1/1` | Numerator and denominator in `[1, 2^31 - 1]`, value in `[1/1000, 1000]`. Reverse playback uses Time Remap |
| `contentExtent` | `Unbounded`, `Fixed{duration, frameRate}`, or `Source{nodeId}` | `Unbounded` | The content interval `[0, E)` the out-of-extent modes refer to |
| `beforeMode`, `afterMode` | closed enumeration `None`, `Hold`, `Loop`, `PingPong` | `None` | Inert while the extent is `Unbounded` |

`Source{nodeId}` resolves at compile time from the referenced source: a sequence's member span at the
composition rate, a video's frame count times its stream period, an audio asset's sample count over
its rate, or a referenced composition's duration and rate, each shifted by that source's own constant
`startFrame` where it has one. An unresolvable reference (missing asset, unknown node, a composition
source whose offset or scale is animated or not identity) compiles as `Unbounded` with a node-addressed
warning. A relinked asset of a different length therefore changes the extent without a document edit.

Time Remap is an optional, animatable Scalar parameter `bloom.layer.time-remap` (seconds of content
time) on the Layer Output node. Absent means disabled. Layer Output node schema advances 4 -> 5 to
declare it; the parameter is not required, so no record is inserted into existing layers.

`inPoint < outPoint` is the only range rule the document validates (both finite; an explicit
`outPoint` must be greater than zero so the existing zero full-duration sentinel stays unambiguous).
The `[0, duration]` containment rule moves from document validation to the gestures (section 6), which
also removes the refused-duration-change defect.

### 3. Out-of-extent modes reuse one mapping core

`composition_time.cpp` grows one shared core, `mapTime(t, linear, extent, before, after)`, built from
the existing checked helpers. With extent duration `E` and last frame start `L` on the extent's frame
grid:

| Region | None | Hold | Loop | PingPong |
| --- | --- | --- | --- | --- |
| `r < 0` | absent | `0` | `r mod E` (non-negative) | reflect, period `2L` |
| `r > L` (Hold) / `r >= E` (others) | absent | `L` | `r mod E` | reflect, period `2L` |

A one-frame extent holds its only frame. `mapCompositionTime` becomes `mapTime` with
`before = Hold` and `after = loopMode`, and must reproduce its current results bit-for-bit (pinned
by the existing `composition_source_tests` plus new equivalence tests). Absent content means the
Layer Output publishes no image slot and contributes silence, exactly like a time outside its
activity window. Float64 remap values convert to exact rationals with the existing binary64
decomposition; an intermediate that overflows int64 falls back to the existing nanosecond
quantization, and a result outside representable time is an `ArithmeticOverflow` diagnostic.

### 4. One composition rule with source-level timing

Source-level timing is retained and composes inside the layer mapping:

```text
source time = sourceMapping(c)
```

`startFrame`/`loopMode` on image, video and audio sources, and `timeOffset`/`timeScale`/`loopMode` on
composition sources, apply to the content time their scope supplies. They are not migrated, because a
source can feed several layers, an effect chain, or a Merge slot with no layer at all, where it is the
only timing that exists. Commands that create a layer from media leave the source at identity
(`startFrame = 0`, `Hold`, offset 0, scale 1) and put all timing on the layer; the Timeline edits only
layer timing.

Existing projects decode with identity layer timing and `Unbounded` extent, so every scope collapses
to the composition clock and every frame, audio sample and cache key is unchanged.

### 5. Evaluation: time scopes in one plan

The compiler assigns every compiled image and value operation to a time scope:

```text
CompiledTimeScope = { parent: TimeScopeIndex?, layerId, mapping: CompiledLayerTimeMapping }
```

- Scope 0 is the plan's own clock. A Layer Output with identity timing (start 0, speed 1, no remap,
  `Unbounded` extent) creates no scope; its upstream stays in the placement scope. Migrated projects
  therefore compile to exactly today's operation list.
- A non-identity Layer Output creates a scope whose parent is its placement scope. Nested layers
  inside a layer's upstream chain scopes, as nested compositions already do.
- An operation reached from several scopes is compiled once per scope. Each copy keeps its source
  node identity, so the operation cache content key (which carries source-node identity, resolved
  operands and, for time-dependent work only, exact time) shares invariant copies outright and
  shares time-dependent copies whenever two layers map to the same content time. The mapped time is
  the cache identity for time-dependent work; no new cache is introduced.
- Preflight resolves each scope's `tau` and `c` top-down once per request, then samples curves and
  evaluates value outputs once per distinct time a scope needs. Value nodes are pure, so a value node
  driving both a layer parameter and an upstream source is evaluated at both clocks.
- A parented child resolves its parent's matrix at the parent's own layer time derived from the
  child's placement time; the compiler references (and if necessary clones) the parent boundary in the
  child's placement scope.
- Storage preflight counts copies as ordinary operations. The scope count and post-clone operation
  count are capped; exceeding either is the compile diagnostic
  `bloom.runtime.compile.time-scope-limit`, never a silent fallback.
- Cancellation, ROI, proxy resolution, pixel budget and cache bypass flow through scopes unchanged.

Plan semantics advances 8 -> 9: operand sampling and media frame selection are now relative to an
operation's scope, and `CompiledLayerOutput` range fields are in placement-scope time, so a version-8
consumer would misread a version-9 plan. Animation sampling (2), evaluator (10) and image primitives
(7) are unchanged; existing pixel goldens must not move. Output identity goldens are re-derived
through the independent identity oracle after it reproduces the current plan-8 pins.

### 6. GPU preparation and final render

The mapping is host parameter resolution, not a pixel operation, and adds no `CompiledOperation` or
`ImageEffectKernel` alternative. The shared GPU scene builder walks the same scopes, prepares each
copy with its scope-resolved operands, and selects media frames at the scope's content time;
`gpu_scene_nested.hpp` calls the shared mapping core. `gpu_coverage_contract.hpp` gains Required
feature axes `feature.layer.time.offset_speed`, `feature.layer.time.remap`,
`feature.layer.time.extent_modes` and `feature.layer.time.shared_upstream`, each with CPU-parity
fixtures on every required route (Viewer, RAM preview, still, sequence, video, headless). There is no
exception, preview-only path or CPU-labelled-GPU frame.

### 7. Commands and gestures

- `SetLayerTiming{startTime, inPoint, outPoint, speed}` is the one atomic timing operation; every bar
  gesture commits exactly one of it per release. `SetLayerTimeModes`, `SetLayerContentExtent`,
  `EnableLayerTimeRemap` and `DisableLayerTimeRemap` complete the surface. Enabling remap creates the
  parameter with two Linear keys mapping `tau = 0 -> 0` and `tau = E -> E` (or the layer span when the
  extent is `Unbounded`); disabling removes the parameter and its curve. All have exact undo/redo,
  respect `locked`, and run through the shared registry for Python, `bloom-cli` and MCP.
- Body drag moves `startTime`, `inPoint` and `outPoint` by the same frame-snapped delta. Edge trim
  changes one endpoint. Alt-drag on the body slips (`startTime` only). Alt-drag on an edge stretches
  about the opposite edge: speed becomes `speed * oldFrames / newFrames` exactly and `startTime` keeps
  the anchored edge's content time fixed; a result outside the speed bounds or term range is refused
  for the whole gesture. Snapping, Shift, Escape and the stale-revision cancel are unchanged.
- Gestures keep at least one frame of the range inside `[0, duration)`; trims may extend an endpoint
  outside the composition only as far as the content extent reaches there.
- `AddImageLayer`, `AddAudioLayer` and the composition drop set `contentExtent = Source{source}`,
  `inPoint = startTime = 0`, and `outPoint` to the frame boundary at or after the extent end, capped by
  the composition duration. A still image stays `Unbounded` and full-duration.
- `SplitLayerAtTime` copies timing to the tail unchanged, so the output-equivalence proof holds.
- Keyframe commands admit any finite representable time; the Timeline clamps key moves to the
  composition interval projected onto the key's clock. Playhead-relative key insertion converts the
  session time forward to the parameter's clock; inserting where content is absent is refused with a
  message.

### 8. Projection and footprints

A Qt-free projection in `src/runtime` answers, per layer, the forward map, the exact inverse
`t = startTime + tau / speed` for layer-clock keys, and the media extent interval in placement time
for the bar. Upstream keys project through the same linear inverse; with Time Remap or a wrapping mode
active their lanes are marked as content time.

`affectedTimes` footprints: trim publishes the symmetric difference of the spans; body drag, slip,
stretch, mode, extent and remap edits publish the union of the old and new spans clipped to the
composition. A layer-clock key edit projects through the inverse; an upstream edit under remap or a
wrapping mode publishes the whole span; an upstream node shared by several layers publishes the union
over all of them. These use the render-reachability helper (#355) once it lands.

### 9. Audio

Audio follows the identical mapping per output sample: the clip's ordered mapping list gains a layer
entry in front of any nested-composition entries. Speed and remap are varispeed (pitch follows rate);
`None` is silence. Mixer, level and loop details belong to #354.

## Consequences

- A layer bar moves its content, keys move with it, media layers start at the media length, and the
  same model drives pictures, audio, preview and export on all three platforms. The logic is Qt-free;
  Linux qualification is local, macOS and Windows use the existing supported CPU fallback until their
  GPU qualification.
- A shared upstream node costs one evaluation per distinct mapped time, not per layer, but plan size
  grows with sharing; the cap makes that bound explicit.
- Two timing layers exist (layer and source). The composition rule is fixed and documented, and
  layer-created media keeps the source at identity, so artists normally see only layer timing.
- Plan semantics moves to 9 and identity digests change, while migrated output is bit-identical.
- Document schema advances by one additive, version-only step (identity defaults; numbering is
  serialized with other lanes) plus the Layer Output node 4 -> 5 version rewrite.

Primary references: [`animation-and-time.md`](../architecture/animation-and-time.md#layer-time),
[`layer-graph-model.md`](../architecture/layer-graph-model.md#layer-timeline-contract),
[`interaction-model.md`](../ux/interaction-model.md#timeline), and
[`evaluation-primitives.md`](../architecture/evaluation-primitives.md#operation-memoization).
