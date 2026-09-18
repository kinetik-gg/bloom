# Animation, Time, And Interaction

Status: accepted

Updated: 2026-09-17

## Geometry and transform levels

- **NATIVE SIZE** is the source geometry in its own units: rectangle width/height, text box,
  font size, line endpoints and path anchors. Stroke width, corner radius and points are authored
  in these units; changing native dimensions does not stretch them.
- **LOCAL TRANSFORM** is the layer's Position/Anchor/Scale/Rotation in parent space. Scale is an
  animatable percentage that stretches the finished shape, including its strokes. Vector Scale
  is edited only through Properties or timeline fields, never through handles.
- **WORLD TRANSFORM** is LOCAL TRANSFORM composed with every ancestor. It is derived, never
  authored. The gizmo works in this space and maps pointer motion back through the parent.

## Purpose And Ownership

This document defines Bloom's first durable animation model, exact sampling semantics, session time,
and direct-manipulation boundary. It specializes the canonical parameter and command model in
[`layer-graph-model.md`](layer-graph-model.md); it does not create a timeline-owned copy of values.

- `src/core` owns exact `RationalTime` comparison and portable rational-to-binary64 rounding.
- `src/document` owns curve/key declarations, typed parameter sources, validation, and allocation.
- `src/commands` owns typed mutations, transactions, and undo/redo.
- `src/runtime` owns immutable compiled curve tables and sampling for an explicit request time.
- `src/ui` owns current time, playhead presentation, unfinished interactions, and request-scoped
  preview overrides.

No layer, node, timeline, Viewer, Properties editor, or add-on gets a private animation store.

## KEY-1 Whole-Component Key Audit

The pre-KEY-1 implementation treats a vector or colour key as one value at one time. The
whole-value assumptions are deliberately recorded here before the component-curve change:

| Area | Whole-value assumption | Component-curve consequence |
| --- | --- | --- |
| Durable model | `Vec2Keyframe` and `Color4Keyframe` store one complete value; `AnimationCurveRecord` has scalar, Vec2, and Color4 alternatives. `Vec3d` is constant-only. | Vector and colour records now contain independently addressable scalar component curves (X/Y, X/Y/Z, or R/G/B/A); the animation source retains a typed default value for components with no keys. |
| Sampling | `runtime::sampleAnimationCurve()` has one interval factor and mixes both vector channels or all four colour channels together. | Sample each component curve independently, preserving Hold, Linear, and EaseInOut per component; an empty component follows the animation source's default value. |
| Commands | `SetKeyframeAtTime*`, Insert/Update/Delete/Interpolation operations are typed by whole curve kind. Batch `KeyframeAddress` identifies only `{curveId, keyframeId}` and `KeyframePaste` carries a whole typed value. | Add a component address and component command surface; parameter-level operations remain all-component conveniences, while batches accept component-scoped selections. |
| Session | `toggleKeyframe*()` and `keyframeDiamondState*()` answer one binary state for a parameter's one curve. `KeyframeSelection` and clipboard data carry only curve/key IDs; effective vector/colour readers sample one whole curve. | Add component toggle/state and all/some/none aggregate state; selections carry their component, and effective readers resolve component curves without changing the public effective-value result types. |
| Timeline lanes | `TimelineKeyframeRow`, grid projection, hit-testing, drag ghosts, and selection dispatch in `src/ui/timeline_ruler.*` and `src/ui/timeline_keyframe_gestures.cpp` project one lane per parameter curve. Property rows read Vec2 as two fields and one diamond. | KEY-2 owns component lanes and component diamonds. KEY-1 changes only the session seam and leaves widget wiring untouched. |
| Compiler/evaluator | `CompiledVec2Curve`/`CompiledColor4Curve` tables are indexed once per parameter; snapshot lowering and CPU preflight/sample/resolve paths assume one whole-value curve and one segment identity. | Compile component tables and sample each component, composing the same typed value for consumers while keeping curve identity and semantics versions stable. |
| Persistence | Canonical JSON emits `kind: "vec2"`/`"color4"` records with whole-value keys; decode validates those shapes; schema 1.3 introduced Color4; production migration currently ends at 1.8. | Schema 1.9 writes flat component keys and optional animation defaults. The deterministic 1.8 → 1.9 DOM migration splits each legacy vector/colour key at the same exact time and interpolation, preserving sampling and reserving generated key IDs above the old high water. |
| Identity goldens | Runtime sampling tests pin the sampling version and vector behaviour; output-analysis and process-frame identity tests pin evaluator/sampling versions and derived digests; UI golden images cover the existing whole-property indicators. | Re-run the identity checks on the final commit. Do not change evaluator or sampling versions when migrated-document samples remain bit-identical; document any justified change and re-derive affected goldens only if required. |

The implementation touch points are therefore `src/document/animation.*` and parameter schema
predicates, `src/commands/animation_operations.*`, `src/runtime/animation_sampling.*`, curve
compilation, snapshot lowering, and CPU evaluation, project canonical encode/decode plus the
document migration registry and schemas, and `src/ui/composition_session.*` with its tests. KEY-2 now supplies the component-aware widgets and lanes described below.

## Finite Value Invariant

Every Float64 an animation curve holds -- a keyframe value, and a 1.12 ease handle's time fraction
and value offset -- is finite. NaN and the infinities are not authorable, not storable and not
writable:

- the commands refuse one with `OperationIssueCode::InvalidValue` (every keyframe insert, update,
  set-at-time, paste, handle edit and value edit; a whole-value vector or colour argument is split
  across its components and each component value is admitted on the same terms)
- `AnimationCurveStore` refuses one on insert and update, and reports one from `validate()`, so a
  bypass of the commands is caught by `Document::commit()` and by the test suite rather than
  reaching a save
- the gestures that compute these values -- the viewer transform drags and the graph editor's value
  and ease-handle drags -- decline to offer an unrepresentable result at all, keeping the last
  representable preview instead of proposing a value the commands would then refuse (see
  "Direct Manipulation And Preview Overrides")
- the canonical writer and decoder refuse one with a typed error rather than terminating (see
  docs/architecture/project-format.md, "Non-Finite Values")

The invariant is deliberately enforced at every layer rather than at one: an unrepresentable value
that reaches the save writer costs the artist a failed save, and one that reached a file would cost
them the project.

## Durable Type Model

`KeyframeId` is a project-global strong ID with allocator and high-water semantics identical to the
other durable typed namespaces. A key keeps its identity when its time or value changes, and deleted
IDs are never reused.

A composition owns an `AnimationCurveStore` containing records ordered canonically by
`AnimationCurveId`:

```text
KeyframeInterpolation = Hold | Linear | EaseInOut
ScalarKeyframe       = { KeyframeId, RationalTime, Float64, outgoing interpolation }
ComponentCurve       = ordered sequence of ScalarKeyframe
Vec2Curve            = { X: ComponentCurve, Y: ComponentCurve }
Vec3Curve            = { X: ComponentCurve, Y: ComponentCurve, Z: ComponentCurve }
Color4Curve          = { Red: ComponentCurve, Green: ComponentCurve,
                         Blue: ComponentCurve, Alpha: ComponentCurve }
AnimationCurveRecord = ScalarCurve | Vec2Curve | Vec3Curve | Color4Curve
AnimationCurveSource = { curveId, optional typed defaultValue }
```

Vector and colour keys are no longer whole values. Each component has its own strictly increasing
exact-rational timeline, key IDs, and outgoing interpolation; component timelines may be empty,
but a curve has at least one key overall. Two keys at the same normalized time are invalid within
one component. Values are finite, and colour alpha remains in `[0, 1]`. The final key of every
non-empty component is canonical `Linear` on every mutation. A component with no keys samples the
typed `AnimationCurveSource.defaultValue`, which is the parameter's unkeyed constant component.

There is no whole-value key record and no derived whole-value list beside the components. "The
parameter's keys" is the UNION of its component key sets, computed by whoever asks: a lane, a
collapsed layer summary, a diamond, a paste. Nothing stores that union, so no writer has to keep a
second copy of the same facts in step with the first.

A `Color4Curve` carries straight/unassociated authoring RGBA, the same encoding a constant solid or
text color already uses. A colour key is valid on exactly the terms `core::Color4d::isValid()`
states: finite RGB -- negative and HDR channels included -- and alpha within `[0, 1]`. Finiteness is
the store's own rule for every component key; the narrower alpha domain belongs to the parameter
that owns the curve, so it is enforced by the animation commands and by
`validateAnimationCurveReferences()`. Animating a color therefore cannot reach a value a constant
one could not.

Curve ownership is deliberately narrow:

- every animation-curve parameter source resolves to one curve in the same composition
- the curve value kind matches the parameter schema exactly
- every curve is referenced by exactly one parameter
- position, anchor, and scale accept a `Vec2Curve`; Vec3 value parameters accept a `Vec3Curve`;
  rotation, opacity, and text size accept a `ScalarCurve`; solid color and text color accept a
  `Color4Curve`
- text content declares animation unsupported: a String has no interpolation
- driver sources remain preserved document concepts but are unsupported by this evaluator

Missing, orphaned, multiply referenced, cross-composition, kind-mismatched, or unsupported curves
make the draft invalid. Diagnostics identify the `ParameterId` and `AnimationCurveId`; key-specific
failures also identify the `KeyframeId`.

Curve and key declarations, rather than references, participate in project-global ID uniqueness and
allocator validation. Rejected drafts do not consume an ID.

## Sampling Semantics Version 1

Sampling first validates the curve/plan, request time and interval invariants, finite values, and
the reference Float64 environment, in that order. It then validates interpolation mode/domain and
the result. The environment check applies to exact-key and Hold paths as well as Linear so
acceptance cannot vary with the selected branch. Interval selection uses normalized `RationalTime`
comparison only:

1. At or before the first key, return the first value.
2. At or after the last key, return the last value.
3. At an exact key time, return that key's stored value bit-for-bit.
4. For an interior `Hold` segment, return its left key on `[left, right)`.
5. For an interior `Linear` segment, compute the exact rational factor
   `(time - left) / (right - left)` for each component curve and apply Float64 scalar Mix version 1
   to that component. Different components may therefore be at different key intervals.
6. For an interior `EaseInOut` segment, transform that component's factor before mixing, exactly as
   described below. An empty component returns its typed default value.

The interval factor is derived as an exact non-negative rational. Products and differences of valid
signed 64-bit rational components require at most 256-bit unsigned magnitude. Bloom therefore uses
a private fixed-width multiword `UInt256`, normalizes the quotient, and rounds directly to binary64
with round-to-nearest, ties-to-even. Exact endpoints produce exact `0.0` and `1.0`. The algorithm
does not route through seconds, `long double`, compiler-specific extended integers, or `libm`.
Overflow, an invalid denominator, an unsupported floating-point environment, or a non-finite mixed
result is a structured evaluation failure.

### Ease In-Out

`EaseInOut` is a cubic Bezier ease whose control points are the two keys' own handles, described
under **Ease handles** below. Their DEFAULTS are the symmetric `(1/3, 0)` and `(2/3, 1)`, and those
default x handles are what make the mode exact and allocation-free: a cubic Bezier whose x control points are
`0, 1/3, 2/3, 1` has `x(s) == s` identically, so the exact rational interval factor IS the curve
parameter and no root finding or iteration is needed. The eased factor is the closed-form polynomial
`3t^2 - 2t^3` of that factor, which is exactly `ScalarPrimitive::Smoothstep` over the unit range --
so the transform reuses an already-versioned core primitive rather than a private multiplication, and
the sampler keeps its "no `libm`, no `long double`, no compiler-specific extended integer" property.

Endpoints stay exact: the transform maps `0` to exactly `0` and `1` to exactly `1`. At the interval's
exact thirds it is exactly `7/27` and `20/27`, and at the exact midpoint exactly `1/2`. Only the
segment's LEFT key decides its mode, so a `Hold` key is unaffected by an eased neighbour.

`kAnimationSamplingSemanticsVersion` is `2`. It enters compiled-plan compatibility and cache
identity alongside scalar, evaluator, and image primitive versions; it moved from `1` when
`EaseInOut` and the `Color4Curve` table made sampling able to produce values version 1 could not.

### Ease handles

A scalar or component key carries two `KeyframeHandle{time, value}` records: an `outgoingHandle`
governing the segment that starts at it and an `incomingHandle` governing the segment that ends at
it. `time` is a fraction of THAT segment's duration measured from the key the handle belongs to;
`value` is an offset from that key's own value. The defaults are `time = 1/3` and `value = 0`,
which are exactly the fixed handles the pre-handle `EaseInOut` carried. A handle therefore belongs
to one scalar axis, which is why a vector or colour key is addressed through its component: there
is no whole-value key for an offset to move along.

For segment `k_i -> k_{i+1}` with `Δt = t_{i+1} - t_i` and `EaseInOut` on the LEFT key, the curve is
the cubic Bezier through

```text
P0 = (t_i,                          v_i)
P1 = (t_i + out.time * Δt,          v_i + out.value)
P2 = (t_{i+1} - in.time * Δt,       v_{i+1} + in.value)
P3 = (t_{i+1},                      v_{i+1})
```

where `out` is the left key's outgoing handle and `in` the right key's incoming one. `Hold` and
`Linear` ignore the handles their keys carry; the handles persist across a mode change so that
returning to `EaseInOut` restores the shape the artist authored. A handle is valid exactly when its
`value` is finite and its `time` lies in `[0, 1]`; the store, document validation and
`validateForSampling` all ask the same predicate, so an out-of-range handle is refused rather than
sampled at some request times and not others.

Sampling branches on a BITWISE default test, not an arithmetic one:

- Both handles of a segment bitwise default: the sampler takes the pre-handle path verbatim -- the
  closed-form `Smoothstep` factor and the shared Mix. Every key written before handles existed lands
  here, which is why `kAnimationSamplingSemanticsVersion` stays `2` and no identity golden moves.
- Otherwise the segment is evaluated as the Bezier above. When both handle TIMES are bitwise
  default the x cubic has control points `0, 1/3, 2/3, 1` and is the identity, so the exact rational
  interval factor IS the curve parameter and no inversion happens; a value handle therefore has a
  closed form (`0 -> 1` with `out.value = +1` is exactly `0.875` at the midpoint).
- A non-default handle TIME needs `s` such that `x(s)` equals the interval factor. Handle times in
  `[0, 1]` keep `x` monotone non-decreasing, and the inversion is a fixed 64-step bisection using
  only `+ - * /` and comparisons in a fixed operation order, with no early exit and no tolerance.
  It is therefore a pure function of its operands: two calls are bitwise equal, and `s` is monotone
  in the request time. The sampler keeps its "no `libm`, no `long double`, no compiler-specific
  extended integer" property.

Compiled curves carry both handles on every compiled scalar and component key, and compilation
copies them verbatim; a handle edit therefore changes the operand a memoized operation was keyed on
and the frame is re-evaluated.

## Compiled Plan And Evaluation

Runtime uses distinct typed operands rather than a per-pixel variant lookup:

```text
CompiledScalarParameter = { ParameterId, Float64 | ScalarCurveIndex }
CompiledVec2Parameter   = { ParameterId, Vec2d   | Vec2CurveIndex }
CompiledColorParameter  = { ParameterId, Color4d | Color4CurveIndex }
```

An immutable plan owns separate scalar, `Vec2d`, and `Color4d` curve tables ordered by
`AnimationCurveId` and contains only reachable curves. Layer Output operations reference all five
compiled transform operands -- position, anchor, and scale from the `Vec2d` table, rotation and
opacity from the scalar one. A Solid's color and a Text's size and color are typed operands too: a
Solid carries one `CompiledColorParameter`, a Text one `CompiledScalarParameter` for its size and one
`CompiledColorParameter` for its color, while its content stays a resolved constant. Indices are
strong types so the wrong table cannot be addressed accidentally.

`kCompiledCompositionPlanSemanticsVersion` is `5`. It moved from `1` when those source fields
stopped being resolved constants, and TEXT-2 moved it again for the compiled text font-reference
and box-layout fields. A plan's semantics version is part of the immutable plan identity, so cached
or persisted frame identities cannot confuse either grammar.

Which parameters may be animated, and with which curve kind, is one set of schema-key predicates in
`bloom/document/parameter.hpp` that document validation, the animation commands, the registered node
definitions, and the snapshot compiler's override gate all ask. A schema that satisfies none of them
is constant-only, so an unknown key can never silently opt into animation, and the set cannot be
widened in one layer and stay narrow in another. A registered parameter definition's
`supportsAnimation` must EQUAL the predicate for its schema key, so a definition can never be a
second opinion about what is animatable.

Value DOMAINS belong to the schema, not to the curve kind, and one shared predicate
(`isScalarWithinSchemaDomain`) answers for a constant, a keyframe, and an interactive override alike.
`bloom.layer.opacity` confines its values to `[0, 1]`; `bloom.text.size` to
`(0, kMaximumTextSizePixels]`; `bloom.transform.rotation` values are any finite number of degrees,
because a rotation curve has to be able to wind past a full turn in either direction. A color value's
domain is the authoring-color contract described under **Durable Type Model**.

The evaluator validates and samples each referenced curve once at request preflight, before any
row kernel runs. Image evaluation consumes the resulting typed constants. Sampling never allocates
per row or dispatches through parameter strings.

## Commands And Source Transitions

The command surface is typed by curve value kind:

- `CreateAnimationForParameter` accepts the exact initial key time, creates a compatible curve,
  seeds the key from the current constant, and uses outgoing `Linear` atomically.
- `InsertKeyframe` rejects an occupied exact time.
- `UpdateKeyframe` preserves `KeyframeId` and rejects a move onto another key's time.
- `DeleteKeyframe` rejects deletion of the curve's final key.
- `SetKeyframeAtTime` updates an exact-time key or inserts one, returns the existing or allocated
  `KeyframeId`, and is the gesture-facing operation.
- `SetKeyframeAtTimeForParameter` is that operation keyed by `ParameterId` rather than
  `AnimationCurveId`, resolving the curve from the parameter's source at APPLY time. It exists for the
  one thing a curve-keyed operation cannot do: sit in the same transaction as
  `CreateAnimationForParameter`, whose curve does not exist until that earlier operation has run
  against the same draft.
- `SetKeyframeInterpolation` re-points one key's outgoing mode, preserving its `KeyframeId`, exact
  time, and value bit-for-bit. It is its own operation rather than a flag on `UpdateKeyframe` because
  the gesture that changes an ease is a menu pick on a key that is not moving. Setting the mode a key
  already carries is a committing no-change; anything but `Linear` on the final key is refused.

### Multi-key commands

`MoveKeyframes`, `DeleteKeyframes`, `SetKeyframesInterpolation` and `PasteKeyframes` are Qt-free
operations in `animation_operations`. Each UI batch is one transaction against the revision frozen
at gesture start (or the current revision for a menu/clipboard action), with one undo entry.

| Operation | Admission and result |
| --- | --- |
| `MoveKeyframes` | Accepts stable curve/key addresses with destination times. Stages and sorts complete curves, permitting selected keys to exchange their original times. Duplicate addresses, occupied final times and times outside `[0, duration)` reject the whole transaction. IDs and values are preserved; final outgoing interpolation is normalized to Linear |
| `DeleteKeyframes` | Removes the complete addressed selection across value kinds. If a curve becomes empty, restores a constant holding its earliest pre-delete key's value and erases the curve atomically |
| `SetKeyframesInterpolation` | Applies Hold, Linear or Ease In-Out to the complete selection. Final keys remain canonical Linear; no effective change creates no history entry |
| `PasteKeyframes` | Accepts original parameter IDs, exact destination times, typed values and interpolation. Allocates new IDs, creates compatible animation for constant parameters, and rejects missing/driven targets, invalid values and occupied times without publishing a partial result |

All four value kinds (Scalar, Vec2, Vec3 and Color4) use the same batch paths. A staged collision does
not consume durable IDs or publish any other curve in the transaction. Undo/redo restores exact
sources, IDs, values, times and outgoing modes. No evaluator, sampling, process-identity or
sampling-version changes are involved.

Endpoint time-stretch submits `MoveKeyframes` with frame-snapped scaled times about the opposite
selected endpoint. That fixed endpoint retains its exact rational time even when it is a subframe;
only moved keys snap to frames. Shift disables ordinary move snapping; the UI converts pointer displacement to
a rational nanosecond offset and applies it with checked portable integer arithmetic. Clipboard
paste uses checked rational offsets relative to the earliest copied key and the current playhead,
preserving subframe spacing. Unrepresentable arithmetic is refused rather than overflowing.

### The Keyframe Gesture

Parameter diamonds aggregate the component curves at the exact session time. A muted outline
means constant; a gold outline means animated with no keys here; a gold half fill means some
components have keys here; a gold fill means all components have keys here. Scalar parameters
use the same forms without half fill. Driven and non-animatable parameters have no diamond.

A parameter click seeds all components from the constant or sampled value when none or some
are keyed, and removes all component keys at that time when all are keyed. Each click is one
transaction. A component diamond keys or removes only X/Y/Z or R/G/B/A; an unkeyed sibling
retains its animation default. Removing the final component key restores the typed source default.

Properties and node cards retain one aggregate parameter diamond and expose a separate diamond
beside every component cell. Timeline vector and colour rows expose component diamonds before
their numeric fields and a second disclosure level for individually named component rows.
A component row edits only its component through the session command adapter, both for constants
and animation; an animated edit at a new time creates only that component's key.

Parameter lanes display the exact union of component times. Clicking an aggregate glyph selects
every component key at that time; clicking a component lane selects only its key. Shift extends
selection. Drag, stretch, duplicate, copy/paste, delete, interpolation and ghost previews preserve
component addresses. Double-clicking an empty component lane inserts only that component.
Collapsed layer summaries read those same component keys.

Editing the VALUE of an animated parameter at a time with no key inserts one, through the same
`SetKeyframeAtTime` path a constant edit's `SetParameterSource` takes. That requires the row to stay
live and to display the curve's sampled value at the current time, which is what the
`effective*Value()` readers provide; a row that showed nothing because its parameter was animated
would make the gesture unreachable.

Converting a constant parameter to animation seeds its first key with the current constant value.
Converting animation back to a constant is an explicit transaction that chooses the constant and
deletes the now-orphaned curve atomically. Editing a driven parameter requires an explicit driver
transition; a gesture never disconnects it silently. One transaction produces one undo entry and
restores exact curve/key records, IDs, sources, times, and values on undo/redo while preserving the
maximum live allocator high-water; history never rewinds an allocator counter. Allocation
exhaustion is a structured command failure and rejected commands do not consume IDs.

### Upstream Value Nodes In The Timeline

A driven parameter has no curve and therefore no key of its own; the key that moves it belongs to the
value node driving it. The timeline shows both halves of that, so retiming driven motion happens where
every other retiming happens.

A DRIVEN parameter row shows the driver node's display name behind a link glyph that selects and frames
that node, and beside it the value the graph resolves the parameter to at the session's current time,
read-only. It shows no editor, because there is no authored value to edit, and it is never an empty
cell. The resolved string comes from one session-owned evaluation shared with the Properties panel, so
the two surfaces cannot show different values for one parameter.

A layer's twirl-down then carries one collapsible group per value node reachable from that layer
through driver links -- breadth-first from the layer's boundary and direct source nodes, deduplicated,
titled by the node's display name, in the same order the Properties panel lists the same nodes
upstream of a selection. A Reroute, the one socket with no parameter behind it, is followed through so
it cannot hide the node behind it. Inside each group the node's ANIMATABLE parameters are ordinary
parameter rows: same diamond, same key lane, same drag, box-select, delete, interpolation and paste
gestures, because they are those rows rather than a second kind that imitates them. A collapsed
layer's key summary is drawn from the same walk, so the summary and the expanded rows can never
disagree about which keys belong to a layer.

The group's collapse key is the node's identity rather than its display name, which two nodes may
share. Editing a row inside a group writes to the value node's own parameter through the ordinary
constant-or-keyframe rule; it does not touch the layer.

## Session Time And Scrubbing

`CompositionSession` owns an exact current `RationalTime` and publishes changes to all editors. It
is not serialized, dirtying, or undoable. Switching compositions resets the session time to exact
zero in version 1. The model may retain an exact time outside the composition work range. A UI scrub
gesture clamps to frame indices whose exact time is in `[0, duration)`, then selects the nearest
index with an exact halfway tie going to the greater index. Frame index `i` maps to exact time
`i * frameRate.denominator / frameRate.numerator`; the maximum is the greatest non-negative `i`
whose mapped time is strictly less than duration. Products and comparisons use checked multiword
arithmetic. Existing subframe keys and direct time entry remain exact and are not rounded
destructively.

Current time is an evaluation-request input, not a persistent preview setting. Every change
immediately advances the desired request generation. Scrub and direct manipulation use
`Interactive` priority; discrete typed time entry, key selection, and document refresh use
`Visible`. Playback serves cached frames immediately, admits predicted fast misses at `Visible`,
and skips other misses as described under **Playback never blocks**. The first Interactive request
submits immediately. Subsequent pointer storms use an injectable 16 ms trailing cadence and retain
at most one active request handle plus one newest pending request per preview owner. A superseded
active request is cancelled but remains active until terminal; only then may the pending request be
submitted. Live parameter overrides at the same revision and time finish the active frame so
continuous input can still display progress. Scrub end bypasses the trailing delay but does not
violate that active-request gate. Scheduler coalescing and stale-result rejection remain
lower-level backstops.

`CompositionSelection` owns a collection of stable `(AnimationCurveId, KeyframeId, Component?)`
addresses and one primary key with a contextual layer. Shift-click and box selection update that shared
collection; vanished keys are pruned after execute/undo/redo and composition changes clear it.
Rows, pixel positions, expanded-layer state, drag previews and the clipboard are session/UI state.
They are not serialized project truth. Timeline property fields bind exact parameter IDs and issue
the same session setters as Properties; the UI never mutates document curves directly.

## Per-Frame Evaluation

### Row Bands

An operation's rows are evaluated in BANDS across a bounded pool of threads the task scheduler owns,
not one after another on the thread that asked. The four CPU row kernels -- solid fill, text coverage,
the affine layer resample, and the layer-stack blend -- and the reference display mapping each write
only their own output row and read only immutable inputs, so rows divide.

The split is `planRowBands()`: a pure function of the row count and the band budget, at least eight
rows per band, remainder spread one row at a time across the leading bands. It depends on nothing
about timing or the order bands finish, so the same image always divides the same way. Pixels do not
depend on the split: a serial frame and a frame banded any number of ways are byte for byte the same,
the pool is not part of `ProcessFrameIdentity`, and no semantics version moves for it. A layer stack's
ENTRIES still fold in order, bottom to top -- only the rows within one entry band.

Progress brackets a row pass (`completed` 0, then the total) instead of counting rows, because a band
runs on a thread the progress callback does not belong to.

A compiled plan is cached per document revision for the same reason a sequence export compiles once:
the compiler is time-independent, so two requests that differ only in time compile to the same plan.
A request carrying an interactive parameter override is compiled directly and never retained.

### Sequence Export

"Export Frame Range..." exports an inclusive range of composition frame INDICES, one complete
publication per frame. It drives the single-frame export's own stages rather than a second copy of
them:

- the plan is compiled ONCE for the whole range, because a compiled plan is time-independent -- every
  animated parameter is a curve index the evaluator samples at the request time -- so recompiling per
  frame could not change a pixel
- every frame is evaluated at its OWN exact rational time (`frameTimeForIndex()`, the same exact
  mapping the ruler and the transport use), never at the session's time and never at an accumulated
  one
- every frame produces its own analysis attempt, its own digest, and its own publication intent
  through `approveFrameExportV1()`; the byte-equality guard is never bypassed
- the artist approves ONCE, from the first frame's completed attempt. Asking per frame would make a
  hundred-frame range a hundred modal dialogs, which is not an approval but an obstacle.
- file names are the destination's stem, a dot, the frame index zero-padded to at least four digits,
  then the destination's extension -- so one range always sorts lexicographically in frame order
- a range containing any frame outside the composition's valid index range is REFUSED, not clamped:
  an artist who asked for frames 0-200 of a 100-frame composition meant something different from one
  who asked for 0-99
- cancellation ends the range and leaves every frame already published exactly as it is. A sequence is
  a sequence of complete publications, not one transaction that could roll back, and the terminal
  outcome says how many landed.

### Viewer Preview Resolution

The preview controller owns an Auto, Full, Half, or Quarter policy. Auto selects the smallest
rendered extent covering the composition's displayed size, including device pixel ratio: Quarter,
then Half, then Full. Each proxy dimension rounds up and stays nonzero. Unknown viewer geometry
uses Full; actual size and larger zooms use Full. Fixed policies ignore zoom. The viewer computes
zoom, fit, painting, and interaction rectangles from the full composition format, so a proxy is
upscaled into the same rectangle without changing composition geometry. A zoom or resize requests
another frame only when its resolved resolution changes.

The Viewer footer places a Resolution dropdown beside Zoom, with Auto, Full, Half, and Quarter.
It defaults to Auto and persists the policy name in QSettings `viewer/resolution`; an unknown saved
value falls back to Auto. The control moves with the editor footer. The footer readout includes
`Auto · ¼`, `Auto · ½`, or `Auto · 1` (or the fixed policy name), followed by exact frame/time.
This reports the requested factor; retained older pixels remain identified by the existing stale
frame status. Proxy painting uses the existing smooth image transform into the composition rectangle.

Policy and resolved resolution are preview request/cache identity inputs, not process semantics.
The existing proxy extent reaches evaluation and display preparation unchanged; identity goldens
and export resolution remain unchanged. RAM preview uses the same controller-resolved factor and
policy for every request and cache lookup. A policy or Auto factor change cancels an active RAM
preview run, keeping completed cached frames and requiring a new run at the new resolution. Single
frame and frame-range exports always evaluate at Full, independently of the Viewer preference.

### RAM Preview

Preview frames are kept in memory so that playing a range a second time, or stepping back to a frame
already rendered, costs a lookup rather than an evaluation.

**Cache key.** Preview render inputs and the requested resolution policy: project, composition,
document revision, exact rational time, preview output, resolution (which is where a proxy factor
lives), resolution policy, quality, color intent, optional ROI, and display-only `ViewAdjust`.
That is `PreviewRequestIdentity` minus its request generation. A cache hit is re-stamped with
the current request generation before publication, so freshness still identifies the request
that the frame answers.

Display qualification is a cache-wide tag; per-viewer exposure/gamma remains in the key. The
qualified display processor publishes once per session, so an entry's display identity can change
at most once, and when it does every earlier entry is stale. A frame whose qualification differs
from the tag clears the cache and adopts the new one.

**What is retained.** The packed RGBA8 display buffer and the identity, and NOT the Float32 process
image it was mapped from. Playback paints the packed buffer and nothing else -- the viewer's own
painting, its display geometry, its colour-state chip, and the direct-manipulation mapping all read
that buffer, none of them the process image. Retaining every process image would spend four fifths
of the budget on pixels used only by occasional reference probes. A retained frame is therefore
about 8 MB at 1920x1080 rather than about 41 MB.

Consumers of scene-linear pixels check `PreparedPreviewFrame::hasProcessFrame()`. The pixel probe
reads an available bounded process frame or schedules exact one-pixel ROI evaluation for a
cached display-only frame. It caches that reference result by frame and position, independently
of display adjustment. Frame and sequence export evaluate their own full-frame requests without
ROI or viewer adjustment. The compiled-plan cache is unaffected by either viewer setting.

**Budget.** `playback/ram-preview-memory-bytes` and
`playback/operation-cache-bytes` are resolved together by the session's one memory-budget ledger.
The usable budget is physical memory less a reserve for the operating system, decoders, and other
applications (40% of physical memory, never less than 8 GiB), and it is the ceiling an explicit
override may reach. The DEFAULT total is capped more tightly still: never more than half of physical
memory, and never more than 80% of what the operating system reported as available at startup. See
docs/architecture/evaluation-primitives.md for the full rule and its per-machine table. Missing,
unparseable, or zero settings use a 60% operation-cache / 40% RAM-preview split of that default
total; the preview side retains its 2 GiB floor when the default total allows it. A single override
keeps its requested value where possible and reduces the other cache; two overrides that overcommit
the machine are proportionally clamped. When physical memory is unavailable or too small to leave
that reserve, the ledger uses the 3 GiB low-memory floor so the preview floor remains useful without
exceeding reported physical memory. When available memory later drops below the reserve, both caches
are trimmed to half their budgets and the status bar says so; the budgets themselves do not change.
The effective operation and preview budgets are shown together in the window status-bar cache cell.
Least-recently-used entries are evicted until each effective budget is satisfied, and a frame larger
than the whole preview budget is refused rather than allowed to evict everything for itself. At
about 8 MB a frame, a 2 GiB preview budget holds roughly 250 frames of a 1920x1080 composition --
ten seconds at 24 fps -- and a RAM preview whose range does not fit stops at the first eviction and
keeps the prefix that does.

**Invalidation is the key.** A document edit advances the revision, so every entry of an earlier
revision is unreachable by construction; those entries are dropped outright when a frame of a newer
revision arrives. Two requests never reach the cache at all: one carrying an interactive parameter
override, whose pixels belong to a gesture rather than to the revision and whose identity cannot say
so, and an explicit refresh, which asks for the frame to be re-derived precisely because something
the key does not cover may have changed.

**The RAM Preview command** (`Ctrl+Shift+Space`, the Composition menu, and the Timeline transport's
own button) pre-renders the composition's work-area frame range into the cache one frame at a time, in
the background, reporting "Caching 42/240" in the Viewer footer and cancellable with Escape, then
asks the transport to play it. Frames already cached are counted without being rendered again, so a
second RAM preview of an unedited range is immediate. The range is the persisted half-open work
area when set, otherwise `[0, duration)`, read through `CompositionSession::workArea()`. Playback
starts inside and loops over those same frame indices. The start frame is included and the end
frame excluded; a range edit cancels an active cache run through the ordinary revision path.

### Background caching

A session-owned background renderer fills the same preview cache while no foreground preview,
interactive scrub, parameter override, pointer drag, or explicit RAM Preview is active. It starts at
the playhead, then visits the next frame, previous frame, and progressively farther frames in both
directions. During playback it visits frames forward from the moving playhead, wrapping at the
composition end. An in-flight frame remains useful when playback advances; the next request is
chosen from the new playhead instead of cancelling slow speculative work on every tick.

Only one speculative frame may be in flight, at scheduler `Background` priority, below `Visible`
and `Interactive`. Foreground preview admission and gesture begin cancel it cooperatively. Its
handle remains the admission gate until terminal, including across revision and resolution changes.
Preparation runs on the CPU executor with the ordinary pipeline's progress, cancellation, diagnostics,
and shutdown handling. It never changes session time or publishes Viewer pixels.

Each pass visits at most the nearest set of frames that fits the cache's byte budget. Cached entries
in that set are reused and protected by the cache's LRU order. The pass then stops, avoiding an endless
cycle that evicts its own frames. A revision, resolution, playhead, or memory-budget change restarts
selection. Old revision entries evict through the existing cache policy. Background caching visits
only frame times inside the session's resolved work area, including when choosing nearby frames
around an out-of-range playhead. Cache budgets, cancellation and shutdown behavior are unchanged.

The ruler paints a thin `Ok` green segment for each retained frame-grid sample at the current
project, composition, revision, resolution, and policy. Subframe samples do not certify a whole frame.
Segment endpoints use `TimelineAxis`; the last ends at the composition duration. Cache mutation
notifications coalesce over 50 ms, including evictions and clears, so filling a burst does not request
one ruler repaint per frame. Revision and resolution changes remove obsolete coverage immediately.

### Playback never blocks

Space and the transport button toggle the same session transport. Space belongs to the window and
works even when every Timeline panel is hidden or replaced; focused text entry keeps the character.
A gesture press pauses playback before its first time change. `Ctrl+Shift+Space` remains the explicit
RAM Preview command: finish pre-rendering the range (or its budget-limited prefix), then play.

Each due tick advances the exact session time and asks for its frame:

- A cache hit publishes immediately without evaluation or coalescing.
- A miss is admitted at `Visible` only when no foreground request is active/pending and the maximum
  observed delivery duration for the current identity is at most half the time until the next tick.
  Delivery timing includes queue, preparation, and UI polling; the extra half provides headroom.
- Unknown, slow, or busy misses are skipped immediately. The previous picture remains visibly stale,
  the frame is counted dropped, and speculative filling continues. A prediction is not a deadline
  guarantee: an admitted frame arriving after its deadline or superseded by another tick cannot
  replace the displayed picture. A valid late result may still enter the cache for a later loop.

The transport retains its two exact clocks. If the next frame is cached, it advances exactly one
frame per due tick, preserving frame-accurate RAM Preview even after a host stall. Otherwise it jumps
to the frame demanded by total elapsed time. Both loop without accumulated floating-point time.
Neither clock waits for cache completion. Explicit RAM Preview is the mode that waits before starting
playback.

### Audio Clock Master

When audio is enabled, a revision-valid `AudioMixDescription` has been published, and the audio
backend starts successfully, `AudioEngine::positionNow()` is the playback master. It is derived from
frames written by the backend callback, the rational play origin, the output rate and playback rate;
the UI transport maps that exact position through `FrameTimeMapping` and never advances audio from
its wall-clock timer. Pausing stops the engine, scrubbing seeks then pauses, and a composition or
revision change clears the old clips before a new mix is accepted.

If there is no resolved audio clip, audio playback is disabled, or the backend cannot start, the
transport falls back to its existing wall-clock frame schedule. That fallback is explicit and keeps
RAM Preview's frame-cache behavior unchanged. Mix publication is revision-guarded, so a completed
worker plan cannot load clips for a newer document revision.

### Dropped-Frame Counter

The Viewer footer reports "N dropped" during playback. The count includes frame indices skipped by
elapsed-time catch-up, uncached targets skipped at admission, admitted requests that fail or miss
their deadline, and superseded requests. Each playback request contributes at most once. Cached
frames contribute nothing. The counter is a count, not a measured frame rate.

`play()` arms and resets counting; `pause()` disarms it and retains the last total. Outside a run the
footer says nothing; during one it reports zero explicitly. Ordinary scrubbing while stopped does
not affect the count.

## Direct Manipulation And Preview Overrides

The Select tool picks the topmost evaluated Layer polygon under the pointer in authored stack
order. Shift-click extends the shared selection; an empty canvas click clears it. The header object
selector, timeline and properties read that same session selection. Picking uses content polygons,
not per-pixel alpha. The primary layer owns the transform gesture; simultaneous multi-layer
transforms remain deferred.

`ViewerMapping` is a frozen value with both screen-to-composition and composition-to-screen
conversion. Its display rectangle includes Fit or custom zoom/pan, pixel aspect and proxy display
geometry. It also captures composition format, evaluation resolution and display descriptor.
Only a frame for the current composition, revision and time can start a gesture. Resize, DPI,
format, proxy, display-descriptor or view-transform changes invalidate the frozen mapping.

A selection box has native-size corner and edge handles, an anchor crosshair, and rotation hit
regions outside its corners. Point text has corner handles only. Hover identifies move, scale, rotate and anchor regions. The viewer takes focus on click.
Gesture begin samples position, anchor, scale and rotation at exact session time, including between
keys; it freezes the revision, target IDs, evaluated local bounds/world polygon and pointer origin.
A missing or singular transform, locked layer, or driven transform parameter refuses the gesture.
No evaluation or media work runs on the UI thread.

- **Move:** convert total screen displacement through `ViewerMapping`, then the inverse parent
  linear transform, and add it to the sampled position.
- **Native size:** Shape and Solid handles edit source dimensions; box-text handles edit the box
  and preserve font size. Point-text corners edit font size uniformly, with no edge handles.
  Edges change one dimension and corners change both; Shift preserves aspect. Line and path
  handles author endpoints and anchors. Stroke width, corner radius and transform Scale remain
  unchanged. Position compensates in parent space to pin the opposite handle; Alt pins the anchor.
  Raster sources without native editable geometry retain transform Scale handles. Vector Scale
  remains available only in Properties and timeline fields.
- **Rotate:** measure the pointer angle around the evaluated anchor in parent space. Shift snaps
  the resulting authored rotation to 15-degree steps. Turns remain continuous across the angle
  seam. Positive rotation is clockwise with Y down.
- **Anchor:** transform the displacement into local content coordinates and change the anchor
  offset. Compensating position in the same gesture keeps the world polygon unchanged.

The parent linear inverse is reconstructed from evaluated polygon edges and the child's sampled
rotation/scale. This includes rotated, mirrored and non-uniformly scaled ancestors and resulting
shear. See [Transform Parenting](layer-graph-model.md#transform-parenting). Pointer updates derive
values from the frozen bases and total displacement, never rounded intermediate edits.

The preview channel is session-only and reusable by future shape/text interactions:

```text
SnapshotCompileRequest.parameterOverrides = [ // at most eight distinct parameters
  { sourceRevision, parameterId, value: double | Vec2d | Vec3d | Color4d | int64 | bool | string | PathValue }
]
```

Admission checks the captured revision, target existence, reachability and unique ownership, then
registered schema, kind and value domain. Every reachable node owner participates, including
Source, Layer, Value, Math and utility nodes. Boolean values are typed booleans; legacy integer
zero/one overrides remain accepted, while other integers are refused for Boolean targets. Duplicate
targets, a ninth override and driven sources are refused. A driver is never hidden or disconnected by this channel.

Accepted overrides become request-local constant records read by the shared lowering functions,
including all source operand kinds. Their dormant curves are omitted. The immutable document is
unchanged; compiled plan and operation caches bypass overridden requests. Preview controller and
pipeline carry the complete vector only on Interactive requests. The first request is immediate;
subsequent requests retain the 16 ms cadence and one-active/one-newest admission policy. Release
or cancellation removes the vector. Completed previews are consumed independently of the slower
Jobs monitor polling interval.

Release writes every changed parameter through the common constant/keyframe/driven command branch
in **one** transaction: `Move Layer`, `Scale Layer`, `Rotate Layer` or `Move Anchor`. Constants are
rewritten, animated values update or insert exact-time keys, and driven edits are refused. One undo
restores all touched parameters and their prior animation state. Zero-change gestures create no
history. Escape, secondary-button cancel, capture loss, window deactivation, resize, composition or
time changes, stale revisions and invalidated mappings clear live state without a command.

Arrow keys nudge the primary layer by one composition pixel; Shift uses ten. Each key event is one
transaction and samples current authored ancestor transforms so repeated keys do not wait for
preview completion. Delete removes the selected layers through the existing RemoveNodes command
in one transaction. All bindings and geometry use Qt's portable input and painting surfaces on
Linux, macOS and Windows.

### Text Editing

`CompositionSession::beginTextEdit` uses the existing value-edit ownership and staging. It freezes
the String parameter, source revision, exact time and initial value. Each accepted input validates
the complete UTF-8 value before replacing the live override. Properties reads that same effective
value. The document and history remain unchanged until one `Edit Text` transaction accepts the
session value; Escape discards the override. No schema or persisted interaction state is added.

The viewer enters through Select double-click, Enter on selected text, or Text-tool creation.
It owns only cursor/selection and native IME composition state. Preedit temporarily replaces the
selected range in the preview string; an IME commit replaces that range in the edit buffer once.
Single-line Enter, multiline Ctrl+Enter and focus loss accept the buffer. Revision, composition or
time changes invalidate it. Properties text fields share the same begin/update/commit/cancel seam.
Canvas editing consumes layer nudge/Delete and tool shortcuts before they reach authoring commands.

A cancellable worker resolves the compiled font and queries the render placement path, retaining
one active request and one newest request. Results from retired generations are discarded. Layout
activity and failures are visible in the canvas, and closing the viewer requests cancellation
without joining a worker on the UI thread. Carets and selection use the layout's byte ranges and
evaluated layer world transform, so parent rotation, nonuniform scale and shear follow the glyphs.
The UI neither rasterizes text nor opens font files. The overlay is absent from output pixels.

### Path Gestures

Path overrides carry the document's bounded `PathValue` (anchors, optional absolute cubic handles,
and closed flag). They obey the same revision, ownership, reachability, kind and domain checks as
other overrides, and bypass frame/plan caches. Path is a constant authoring kind, without a curve.
`liveValue` includes Path values while numeric effective readers retain their existing kinds.

Pen creation holds only a session draft and screen-space outline until close or Enter. A selected
Path anchor/handle drag freezes the evaluated local-to-composition transform and the viewer mapping.
It stages the complete Path through the value-edit seam; the Layer anchor offset compensates for
changes to the geometric bounds centre. Path and compensation commit in one transaction. Alt breaks
handle symmetry; Delete removes the selected anchor in one transaction. Escape, capture loss,
window deactivation, resize, selection, revision, composition and time changes cancel the gesture.
No rasterisation, media I/O or evaluation runs on the pointer event path.

### Value Edits

`CompositionSession::beginValueEdit(parameterId, optionalComponent)` captures the revision, exact
session time and sampled base value. `updateValueEdit(value)` validates and publishes a session-only
value through the same request override channel. `liveValue(parameterId)` and the typed effective
readers return that value immediately; Properties, node cards and timeline rows refresh from
`liveValueChanged`, independently of preview completion. Matching typed setters stage their values
while an edit is active, preserving each surface's units and colour conversion.

Numeric typing publishes every valid finite change; incomplete text leaves the last valid preview
visible. Horizontal scrubbing and arrow steps use the same seam. Release, Enter or focus-out calls
`commitValueEdit()` once; Escape calls `cancelValueEdit()` and restores the base with no transaction.
A component field keys only its component. Linked vector controls and the colour picker edit the
whole value. Animated values keep their curves and author at the captured exact time. Zero-change,
cancelled, stale, time-switched and composition-switched edits create no history. Driven and locked
parameters refuse live authoring. Discrete enum choices remain one immediate command per choice.

Value and viewer-transform overrides share the existing Full/Half/Quarter/Auto resolution policy.
Auto follows the fitted viewer scale, using the same proxy mapping as viewer manipulation.
Completed live frames can be presented while newer input is coalesced, so continuous motion does
not starve feedback. Committing always requests the configured resolution and reference quality.
These request-only choices never change durable state, compiled-plan grammar, semantics versions
or committed pixels. Override plans and frames remain excluded from revision caches.

## Required Verification

- live numeric and colour edits request Interactive overrides before any transaction, refresh peer
  surfaces, commit one history entry, and cancel without history
- the first Interactive request submits immediately; completed live frames present during continuous
  input and meet a generous offscreen latency bound independently of Jobs monitor polling
- reachable Source, Layer, Value, Math and utility owners accept each supported override kind, while
  driven sources remain refused and request-local lowering matches authored constants

- exact ordering and interval selection at extreme normalized rationals
- binary64 factor rounding at extreme representable ratios and halfway ties; subnormal key
  values/Mix results are covered separately because signed-64-bit time components cannot produce a
  binary64-subnormal interior factor
- Hold/Linear/EaseInOut endpoints, extrapolation clamps, scalar/Vec2/Color4 sampling, the eased
  factor at the interval's exact thirds and midpoint, and non-finite mix failure
- a default-handle eased curve sampling bit-for-bit as the pre-handle path at many rational times,
  the value-handle closed form, a deterministic and monotone handle-time inversion, an out-of-range
  handle time refused, and an ease-handle edit reaching an already memoized frame
- curve/source/kind/ownership validation and project-global curve/key ID collisions
- create/insert/update/delete/source-transition undo and redo with exact identities
- snapshot/plan equality and cache identity for curve, time, and override changes
- one sample per referenced curve per request and no row-loop allocation
- bounded scrub storms, stale-frame suppression, scrub-end flush, and session-only time
- commit, cancel, capture loss, mapping change, stale target, driven target, and zero-delta gesture
  behavior
- the keyframe gesture's four transitions, their one-transaction undo granularity, and the three
  diamond states agreeing with the document at every frame
- every animatable schema reaching the gesture, and text content refusing it
- stepping frame by frame over an animated composition and sampling each frame's own exact value
- a sequence export whose per-frame files carry per-frame values, and the dropped-frame counter's
  silence while nothing is measuring
- row-banded evaluation byte-identical to serial at every band width, cancellation observed inside a
  band, and the deterministic band split itself
- a compiled plan reused across requests at one revision and recompiled at the next
- a cached preview key published with no task and no evaluation, eviction under the memory budget,
  and entries of an older revision dropped when a newer one arrives
- a retained frame costing its display buffer alone, painting exactly the pixels the evaluation
  published, and letting its process image go
- a RAM preview of a twenty-four frame composition caching every frame, then playing all of them in
  order with nothing evaluated and no frame dropped, and its cancellation keeping what it cached

Curve modifiers, procedural extrapolation, expression sampling, shared curves, playback audio
sync, multi-layer transform gestures, and onion skinning are explicitly deferred.

## Operation memoization

Compilation marks each image and value operation time-dependent when a curve, Time node or a
driver reaching one contributes to its inputs. Dependence propagates through transforms, Merge
and output; a constant driver does not make an otherwise static branch animated. An invariant
operation omits request time from its memoization key, so a static solid-plus-text composition
runs each of its six image operations once across 24 requested frames, provided the cache can
retain those results. Frame identities still carry the exact requested rational time.

Memoization sits beneath the existing frame cache and is shared by the session's foreground,
RAM-preview and background pipeline. Keys resolve each operation's own operands and input content
hashes, with revision-qualified addresses and cross-revision content reuse. A solid-color edit
reruns the solid, its transformed output, Merge and composition output while the text branch hits.
Half-open layer visibility is an explicit key input even when all authored values are invariant.

The LRU budget is the operation-cache allocation from the shared memory-budget ledger. Drag
overrides bypass lookup and insertion. The window status bar reports operation-cache hits, misses,
retained bytes and effective budget beside the packed RAM-preview frame count and budget. Cache
retention never changes the pixel reference or semantic identity versions. See [Operation
memoization](evaluation-primitives.md#operation-memoization)
for the complete key, ownership, concurrency and budget contract.

### Media disk cache (task CACHE-2)

Distinct from the operation cache above and from the RAM-preview frame cache: a THIRD, disk-backed
cache of decoded source images sits beneath `evaluateImageSource()`'s own lookup into the shared
memory `OperationCache` above (its `OperationCacheEntryKind::DecodedMedia` entries;
`src/runtime/image_source.cpp`, `src/media/cache`). A memory miss consults it before decoding; a
decode-on-disk-miss writes back off the calling thread so scrubbing never waits on the write.
Unlike the memory caches, it persists across restarts, which is what makes a second RAM Preview
run or a reopened project's second scrub pass over the same footage a disk read rather than a
re-decode. It follows the same "never cache overridden/interactive frames" rule as the operation
cache above, gated by the identical `bypassOperationCache` condition. See [`media-io.md`'s "Disk
cache"](media-io.md#disk-cache) for the complete key, format, eviction, and settings contract.

## Animated Parent Transforms And Bounds

A layer samples its own position, anchor, scale and rotation at composition time, then composes
its local matrix with its parent's resolved matrix. This continues through the ancestor chain.
Animated and driven parent values therefore move the child's evaluated content bounds, anchor and
viewer overlay polygon at the same time as its pixels. Cache dependencies include the resolved
parent transform; changing an ancestor cannot reuse stale child geometry or imagery. Parent opacity,
enabled/solo state and time range do not suppress or fade children. The child's own range and opacity
continue to apply independently.


## Composition Source Time Mapping

A composition source samples its offset and scale at the containing composition's time, then uses
`sourceTime = (compositionTime - timeOffset) * timeScale`. Offset is in seconds; a positive offset
delays the source. Scale 1 plays at normal speed, 2 plays twice as fast, and a negative scale reverses
the direction around the offset. Scale 0 freezes the first frame. The referenced composition's own
frame rate determines its final frame start, independently of the parent's frame rate.

As with image sequences, negative mapped time holds the first frame. Hold clamps positive time to
the last frame start. Loop wraps at the half-open composition duration. PingPong reflects around
the last frame start, with a period twice that time, so endpoints are not duplicated. A one-frame
composition holds its only frame. The evaluator preserves rational time through checked arithmetic.
If Float64 denominators overflow an exact intermediate, source time is rounded to nanoseconds;
a mapping still outside representable time produces an arithmetic diagnostic instead of wrapping integers.
Nested composition parameters and animation are sampled at that mapped time. Audio descriptions
retain the same ordered mappings for every nested clip, with each enclosing Layer's range and
participation controls. Playback applies the mappings in order to every output sample before
selecting the leaf audio sample. Multiple nested layers contribute separate clips to the sum.
Hold and zero scale repeat the sample at the held time; Loop and PingPong map sample times
continuously through the same boundaries used for image evaluation. No nested audio means silence.

Compiled plans retain nested plans at the same project revision. Mapped time participates in the
composition-source operation-cache key; nested evaluation also uses the evaluator's shared cache.
Time overrides bypass retention throughout the evaluation. The plan grammar is version 6; animation
sampling remains 2, evaluator semantics remains 8, and image primitives remain 7. The independent
identity oracle reproduced plan-5 base digests before deriving plan-6 replacements; existing pixel
pins and all preimage lengths remain unchanged.
