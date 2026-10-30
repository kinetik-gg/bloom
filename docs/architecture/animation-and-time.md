# Animation, Time, And Interaction

Status: accepted

Updated: 2026-10-31

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

`kCompiledCompositionPlanSemanticsVersion` is `9`. It moved from `1` when those source fields
stopped being resolved constants, TEXT-2 moved it for the compiled text font-reference and
box-layout fields, and ADR 0024 moved it to `9` because operand sampling and media selection are
relative to an operation's time clock and Layer Output ranges are in placement-clock time. A plan's semantics version is part of the immutable plan identity, so cached
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
| `MoveKeyframes` | Accepts stable curve/key addresses with destination times. Stages and sorts complete curves, permitting selected keys to exchange their original times. Duplicate addresses, occupied final times and non-representable times reject the whole transaction. Under [layer-owned time](#layer-time) a key's clock is its layer or content clock, so admission no longer requires `[0, duration)`; the Timeline clamps moves to the composition interval projected onto the key's clock. IDs and values are preserved; final outgoing interpolation is normalized to Linear |
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
`Visible`. Playback serves cached frames immediately and admits bounded same-domain cold work at
`Visible`, retaining one active producer and one newest pending intent as described under
**Playback never blocks**. The first Interactive request submits immediately. Subsequent pointer storms use an injectable 16 ms trailing cadence and retain
at most one active request handle plus one newest pending request per preview owner. For scrub and incompatible intents, a superseded
active request is cancelled but remains active until terminal; only then may the pending request be
submitted. Compatible Playback follows the useful-work rule below. Live parameter overrides at the same revision and time finish the active frame so
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

A compiled plan is cached for the same reason a sequence export compiles once: the compiler is
time-independent, so two requests that differ only in time compile to the same plan. The cache entry
retains the immutable input snapshot it was built from, and a request matches only when it carries
the exact same retained project-state object plus the same composition and revision -- not merely the
same numeric `(ProjectId, CompositionId, Revision)` tuple, because every New/Open document
deliberately reuses those numbers. Copies of one snapshot, which is what every request carries, still
hit; a different document with colliding numbers recompiles. A request carrying an interactive
parameter override is compiled directly and never retained.

A rebind to a different document (`documentRebound()`) is fired after the new live and evaluation
snapshots are installed but before the ordinary refresh signals. The foreground preview cancels and
detaches its active handle, clears any pending request and the frame cache, and resets its displayed
and request state there, so an old document's queued completion can never publish or cache pixels
under a colliding numeric key; the normal evaluation/composition signals then build the new
document's request. RAM preview and background caching already cancel on those signals and discard
their old completions the same way.

### Sequence Export

A job in the export queue (a custom range) exports an inclusive range of composition frame INDICES, one complete
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

**What is retained.** Display-only buffers and their exact identities, never the Float32 process
image solely for replay. Host-packed products retain RGBA8 pixels; the native resident route instead
retains an owner-bound lease charged by actual native allocation and metadata. A tight 1920×1080
host buffer has 8,294,400 pixel bytes before metadata; native alignment may charge more.
The new `GpuPreparedPacked` service product retains only sealed GPU-prepared host pixels and proof,
with no native image or process frame. ROI pixel extent is its data window, distinct from the full
display mapping. Ordinary application Viewer/cache use is adopted: painting and restamps borrow
immutable bytes/proof. Actual charge includes vector capacity, proof/envelope, bounds and identity
metadata. Controlled 100-frame Full/Half ranges retain 829,620,400/207,540,400 B under a 1 GiB host
cache with no retained native products. Warm exact-time readiness observations add no evaluation,
GPU dispatch, final transfer or cache growth; they do not prove a repaint at every seek or sustained
clock performance. Channel derivatives run off the UI thread in a bounded cancellable CPU worker
and are labelled CPU presentation; exposure/gamma/OCIO still use genuine GPU display preparation.

Consumers of scene-linear pixels check `PreparedPreviewFrame::hasProcessFrame()`. The pixel probe
reads an available bounded process frame or schedules exact one-pixel ROI evaluation for a
cached display-only frame. It caches that reference result by frame and position, independently
of display adjustment. Frame and sequence export evaluate their own full-frame requests without
ROI or viewer adjustment. The compiled-plan cache is unaffected by either viewer setting.

**Budget.** `playback/ram-preview-memory-bytes` and
`playback/operation-cache-bytes` are resolved together by the session's one memory-budget ledger.
Configured ceilings leave a host reserve and use the default 60/40 split; overrides are resolved
against host capacity without rewriting requested settings. Production reusable retention starts
at zero until an off-UI sample permits it. Foreground preparation remains possible without cache
retention. Visible finite Linux cgroup v2 headroom further clamps reusable budgets, with zero
reclaimable-host credit; hard shrink overrides floors and cooldown. Unsupported process-limit
platforms use explicit host-only best effort, retaining any earlier finite constraint. See
[evaluation primitives](evaluation-primitives.md) for provenance, reserve, recovery and live-work
rules. This is retention policy, not a whole-process memory guarantee. Host pressure trims admission
to 25%, then 10%; it does not free active frame/export references. The status bar separates the
configured cache ceiling from the effective reusable allowance.
Least-recently-used entries are evicted until each effective budget is satisfied, and a frame larger
than the whole preview budget is refused rather than allowed to evict everything for itself. At
about 8 MB a frame, a 2 GiB preview budget holds roughly 250 frames of a 1920x1080 composition --
ten seconds at 24 fps. RAM preview stops on admission refusal or loss of an already retained
requested sample. An eviction of unrelated policy/composition entries does not stop a fitting
range. The surviving samples may start after the requested beginning or contain holes; they are
never described as a retained prefix.

**Invalidation is the key, and the key is the retained evaluation snapshot, not the live document
revision.** `CompositionSession::snapshot()` and `snapshotChanged()` remain the live document and UI
truth: every applied command advances them. Beside them the session retains
`evaluationSnapshot()`, the newest genuine snapshot whose render-relevant content the preview has
evaluated, and publishes `evaluationChanged()` only when it actually moves. A command whose result
proves it changed no rendered pixel (`CommandResult::renderAffecting == false`) leaves
`evaluationSnapshot()` on the previous genuine snapshot, so the prepared frame, the frame-cache key,
the compiled plan and the operation cache keep one real revision instead of one per drag. Every
other command advances `evaluationSnapshot()` to live. The retained snapshot is
never a re-stamped revision: it is one the session actually read from the document, and nested-plan
`sourceRevision` and `ProcessFrameIdentity` keep naming it. A cache entry becomes unreachable when
the session no longer accepts its own revision for its own time, and is pruned then -- never by a
wholesale per-project revision drop. Two requests never reach the cache at all: one
carrying an interactive parameter override, whose pixels belong to a gesture rather than to the
snapshot and whose identity cannot say so, and an explicit refresh, which asks for the frame to be
re-derived precisely because something the key does not cover may have changed. CACHE-1 narrows
this for one derived input: an interactive override plan may still READ an already-verified,
immutable native decoded still-image entry out of the evaluator's memory cache instead of
re-decoding it, but never inserts a source entry on a miss, and derived operation results,
overridden plans, gesture frame-cache insertion, and disk-cache reads and writes stay bypassed. An
explicit evaluation bypass (`request.bypassOperationCache`, distinct from the preview frame-cache
refresh described here) disables even that read-only reuse.

**Which edits are render-neutral (#399).** The command stack does not take an operation's word for
it. After applying a transaction it compares the project before and after through the document's
one field classification, `document::sameEvaluationInputs()`
(`src/document/include/bloom/document/evaluation_inputs.hpp`), and a transaction that changed no
evaluation input is render-neutral whatever its operations declared. Moving, collapsing or resizing
a node card, framing nodes in a group and the group's name, padding or color, a layer's name, label
color and lock, a mask's name, composition name, folder, work area, safe areas, timecode display
and viewer background, the project name, asset names, folders, tags and order, and the export queue
are presentation: saved, undoable and dirtying, but invisible to the plan and to every cache. Undo
and redo replay the impact stored with the history entry, so they are neutral too. Selection,
current time and panel state are session state and never reach the document at all. The comparison
runs on shared immutable storage first, so an edit pays only for the composition it touched. The
proof also checks declarations the other way: a transaction whose operations all declared
themselves render-neutral but which changed an evaluation input fails closed to a whole render.
An operation that publishes its own finite footprint keeps it as published.

A field is classified where it lives. Presentation collections (node groups, asset folders, the
export queue, the project name) are skipped whole, so any field added to them -- a group frame
color, for example -- is presentation by construction. Node layout is presentation except `muted`,
which the compiler reads only through `document::nodeMuted()`, so a new card field such as a
collapse state is presentation by construction too. Records that mix both kinds name their
presentation fields in `evaluationView()`; every unnamed field, including one added later, stays an
evaluation input, so a missed classification costs a re-render and never a stale frame. A name is
presentation only while nothing evaluated refers to it: every evaluation reference in the document
is by stable id, and text an evaluated node resolves by name (a channel expression naming a
channel) is a parameter value and therefore an evaluation input. A feature that makes an evaluated
value read a layer, node or group name must move that name into the evaluation inputs.

**Time-indexed provenance, and two distinct signals.** TEMPORAL-2A replaced the single retained
snapshot with `evaluationSnapshotForTime(t)`: a sorted, full-cover list of spans, each naming the
genuine snapshot whose pixels represent that composition time. A finite changed-time footprint (the
clip `SetLayerRange` symmetric difference of the old and new half-open activity spans) retains the
previous genuine snapshot OUTSIDE its changed intervals and switches only those intervals to the
live snapshot. Every consumer therefore resolves the snapshot FOR ITS OWN TIME: the frame-cache key,
the foreground committed request, the live-session guard, the analysis request, the viewer's gesture
mapping, and each RAM/background submission. Nested `sourceRevision` is never rewritten, and each
retained frame keeps the real revision and plan it came from.

The signal split is what makes this user-visible rather than merely retained. `documentEvaluationChanged()`
says a DOCUMENT command moved the time-indexed provenance: the foreground preview re-scopes cache
retention and PRESERVES the displayed frame (and in-flight work) when that frame's time still
resolves to the same genuine snapshot, otherwise it rebuilds only the changed interval, answering
from another retained segment's cache entry when possible. RAM and background rescan and submit only
frames the new provenance no longer accepts; a unitary live reset (a whole-render or unknown edit)
cancels the RAM run as before. `evaluationChanged()` is reserved for NON-document transitions whose
snapshot revisions are unchanged but whose pixels are not -- the qualified display transform becoming
available, a colour-settings change, and rebind -- and still forces a fresh derivation; that is why a
"same revision" test can never be used to keep a display-qualified frame.

**Split reuse (SPLIT-2).** An output-equivalent `SplitLayerAtTime` publishes a deliberately EMPTY
finite pixel footprint plus ordered `LayerIdentityRemap`s (original->tail over `[split, originalOut)`).
The session composes those remaps onto the time-indexed segments instead of resetting whole-live:
segments are subdivided at each remap boundary, a segment coalesces only when it has the same actual
snapshot owner AND identical mapping state, and a mapping composes onto existing mapping VALUES so a
repeated split of a tail collapses original->intermediate->final rather than naming an intermediate
ID absent from the retained snapshot. A finite pixel-interval replacement switches only the changed
intervals to live (clearing their mappings); unaffected segments keep their snapshot and mappings.
Undo publishes the already-inverted list, so `before` is validated against the PREVIOUS live graph
and `after` against the new one. Whole/cap/rebind fall back to whole-live with no mappings; a
composition switch keeps the spans and mappings it retains (see below).
`CompositionPreviewController::currentLayerBounds()` is the one place viewer paint, hit-test, text
editing and selection read geometry: it translates each retained bound's layer/node ID to the current
graph for the frame's own time, so a cached tail frame hit-selects and drags the NEW tail while the
head is untouched. This is metadata only -- no pixel copy and no reevaluation; the retained frame
keeps its genuine snapshot, revision and process identity. Non-merge/parent/driver cases stay
conservative (whole-render, no remaps).

**Deletion.** TEMPORAL-DELETE classifies `RemoveNodes` from the graph BEFORE erasing. When every
removed node is an ordinary layer boundary whose output feeds only the Merge stack, no surviving
child is parented to a removed boundary, no surviving parameter is driver-bound to a removed node,
no removed boundary is solo (removing a solo layer can unsuppress others), and the removed set is
not empty of merge participation, the changed time is the normalized union of the removed boundaries'
half-open active spans (absent `outPoint` means the composition duration). An isolated boundary
contributes nothing, and an all-isolated set publishes a deliberate empty footprint. Anything else —
a source, effect, reroute, or value node; a non-merge downstream consumer; a dependent child; a
driver dependency; a solo layer — leaves the whole-render default. The result rides the same
`documentEvaluationChanged`/cache-retention path as a clip trim, so unaffected cached frames are
retained with zero preparation and only the deleted span re-derives.

**Reachability-proven edits (#355).** Node and parameter commands consult the shared
[render reachability](layer-graph-model.md#render-reachability) helper. `AddNode`, `RemoveNodes`,
`ConnectPorts`, `DisconnectInput` and `DissolveNode` capture the composition's reachability before
mutating and publish a deliberate empty footprint when no node they touched (the node itself, every
consumer of a removed output, a Merge losing a slot, a driven parameter's owner, a surviving parent)
is reachable before or after and the compiled topology (nodes, edges, nested references) is
unchanged; deleting a disconnected Blur, Solid or value node therefore keeps every cached frame. A
reachable topology change stays whole-render, because it can change compile validity everywhere;
`RemoveNodes` still falls back to the TEMPORAL-DELETE layer proof. `SetParameterSource` and every
keyframe/animation operation declare `Operation::parameterEditScope()`; the command stack then diffs
that composition's parameters and curves ONCE per transaction (`Work::parameterFootprintDiffs`). A
changed constant or source changes the whole duration; a changed curve changes, per component, only
`[previous key, next key)` around each added, removed, moved or re-valued key (holds extend the first
and last key to the composition's ends, and handles and interpolation never reach past a neighbour).
That time is intersected with where the parameter's owner can reach output: a gating layer's span,
the whole composition when it reaches Composition Output ungated, nothing when it is unreachable
(solo-excluded, disabled, a later slot of a muted Merge, or unconnected). A changed driver binding,
a graph or mute change in the same transaction, an unproven dependency (a readout), or a different
duration fails closed. Keys are composition times; layer-owned time mapping must map them before it
may rely on this proof.

**Per-composition retention (#355).** Switching composition no longer discards the composition
being left: `CompositionSession` keeps its spans (with their mappings) for up to
`kMaxRetainedCompositions` (4) inactive compositions, and restores them on return when they still
form a full cover whose snapshots agree with the live project, duration and frame rate; otherwise
the entered composition starts whole-live. The preview controller pushes the retained spans to the
frame cache's accepted-provenance policy beside the active ones, so those frames are not pruned and
a return answers from the RAM preview with no preparation. Every committed result is applied to the
retained compositions too: a finite footprint of a retained composition is applied in place; a
non-empty footprint, or a whole render, of another composition drops a retained (or the active)
composition only when that composition renders it through a reachable nested Composition Source --
checked against the live snapshot and every snapshot it still retains, via
`document::compositionsDependingOn()`. `CommandResult::affectedComposition` names the one
composition a whole-render transaction was confined to (from each operation's footprint or
`Operation::renderScope()`); without it -- project-wide edits, mixed compositions, stale or
external revisions, rebind -- every retained composition is dropped. A footprint naming a
composition the live project lacks is inconsistent and resets. `retainedEvaluationChanged()` tells
the controller to re-scope cache retention when only retained compositions changed.

**Cache retention.** The cache holds an accepted-provenance policy: while a composition's time is
inside a span, only that span's revision may be retained there. A finite edit prunes exactly the
entries whose own revision is no longer accepted for their time (counted as `staleDrops`) while
unaffected retained segments survive untouched; a work-area exclusion stays `rangeDrops`, never an
eviction or pressure drop. `timesFor()` matches everything deciding pixels EXCEPT the source
revision, so the timeline's cached markers span multiple retained revisions. The compatibility
accessor `evaluationSnapshot()` remains, returning the uniform snapshot or the live snapshot when
provenance is mixed. `ViewerEditor::currentMapping()` and the controller's live-session guard accept
a frame whose project, composition and time agree and whose revision is either the live revision or
the accepted snapshot FOR THAT TIME, so a gesture still maps after an edit while an old project's
frame can never become current merely because a numeric revision collides. An explicit refresh, a
resolution change and a display/view change still re-derive; a retained segment is a cache identity,
not a promise that a forced request will be answered from it. Note that an explicit refresh is NOT a
"current live revision" oracle: it also resolves `evaluationSnapshotForTime(t)`, so at an unaffected
time it re-derives from the RETAINED snapshot. Only a request built from `session.snapshot()`
carries the live revision, which is why pixel-parity tests prepare that live oracle explicitly rather
than reusing the refresh path. A finite edit that changes the pending/active request's time must
rebuild that target even when the displayed last-good frame sits at an unaffected earlier time; the
handler therefore inspects the pending/active identity's own time and provenance, never the
displayed frame alone.

**The RAM Preview command** (`Ctrl+Shift+Space`, the Composition menu, and the Timeline transport's
own button) prepares the composition's work-area frame range with at most two requests in flight,
reporting "Caching 42/240" in the window status bar and cancellable with Escape. The numerator is
currently retained exact frame-grid samples, including valid preexisting entries, rather than
insertion attempts. Its tooltip separately reports successful preparations this run and the longest
contiguous cached span. Only a still-current fully retained range autoplays, from its beginning;
a budget-limited partial range stops and leaves ordinary manual work-area playback available. Frames already cached are counted without being rendered again, so a
second RAM preview of an unedited range is immediate. The range is the persisted half-open work
area when set, otherwise `[0, duration)`, read through `CompositionSession::workArea()`. Playback
starts inside and loops over those same frame indices. The start frame is included and the end
frame excluded. Start submits the initial bounded batch; terminal completion delivery consumes
ready typed products and advances/refills without waiting for the Jobs snapshot timer. Completion
of child work can wake the bridge too, so wake counts are not frame counts.

A work-area edit (`SetWorkArea`/`ClearWorkArea`) is render-neutral: it scopes which frames a range
command caches, not what a pixel looks like, so it is declared layout-like in the command layer and
never advances the evaluation snapshot or recompiles a plan. It publishes its own
`workAreaChanged()` -- based on the effective half-open range actually changing, not on every
document edit -- and the shared preview cache re-scopes to the live range: entries outside it are
released as `rangeDrops` (never as eviction or memory-pressure, so a trim cannot abort a run as
though the budget were exceeded), every later insertion is refused if it is out of range so a late
completion after a trim cannot resurrect a pruned entry, and the already-displayed frame is
untouched. A frame outside the work area can still be displayed; it is simply not retained. An
active RAM run adapts rather than cancels: the in-flight frame is allowed to land (retained only if
still in range), the run rebases onto the new range, and only missing frames are submitted, so an
expansion or shift fills just the entering interval and a fully cached shrink evaluates nothing.
Background fill follows the same rule through `workAreaChanged()`; the automatic pass, its budget,
priority and yielding behavior are otherwise unchanged, and a range edit never turns itself into an
explicit RAM command.

### Background caching

A session-owned background renderer fills the same preview cache while no foreground preview,
interactive scrub, parameter override, pointer drag, or explicit RAM Preview is active. It starts at
the playhead, then visits the next frame, previous frame, and progressively farther frames in both
directions. During playback it visits frames forward from the moving playhead, wrapping at the
composition end. An in-flight frame remains useful when playback advances; the next request is
chosen from the new playhead instead of cancelling slow speculative work on every tick.
Terminal results are consumed through the shared coalesced completion signal; existing background
idle-start/yield policy is unchanged and separate from Jobs progress snapshots.

Only one speculative frame may be in flight, at scheduler `Background` priority, below `Visible`
and `Interactive`. Foreground preview admission and gesture begin cancel it cooperatively. Its
handle remains the admission gate until terminal, including across revision and resolution changes.
Preparation runs on the CPU executor with the ordinary pipeline's progress, cancellation, diagnostics,
and shutdown handling. It never changes session time or publishes Viewer pixels.

Each pass visits at most the nearest set of frames that fits the cache's byte budget. Cached entries
in that set are reused and protected by the cache's LRU order. The pass then stops, avoiding an endless
cycle that evicts its own frames. An evaluation-snapshot, work-area, resolution, playhead, or
memory-budget change restarts selection; a layout-only edit moves none of those and leaves the pass
running. A work-area edit re-anchors on the playhead and skips cached in-range frames, so it fills
only what the new range adds. Old evaluation-snapshot entries evict through the existing cache
policy. Background caching visits
only frame times inside the session's resolved work area, including when choosing nearby frames
around an out-of-range playhead. Cache budgets, cancellation and shutdown behavior are unchanged.

The ruler paints a thin `Ok` green segment for each retained frame-grid sample at the current
project, composition, revision, resolution, and policy. Subframe samples do not certify a whole frame.
Segment endpoints use `TimelineAxis`; the last ends at the composition duration. Cache mutation
notifications coalesce over 50 ms, including evictions and clears, so filling a burst does not request
one ruler repaint per frame. Revision and resolution changes remove obsolete coverage immediately.

**Retention outcomes and live coverage.** Cache insertion returns `Retained`, `AlreadyPresent`,
`RejectedBudget`, `RejectedStale`, `AllocationFailure`, or `Invalid`, after actual admission and
budget policies. Keys, display-only products and entry capacity are prepared before qualification
or revision pruning; failed allocation and pre-admission refusal leave earlier valid entries and
charges intact. A lease expiring after commit may report stale rejection with actual evictions;
the transaction does not promise to reverse concurrent native lifetime changes.
Lookup membership does not increment hits or touch LRU. A live resident lease certifies servability,
a CPU display buffer certifies CPU retention, and an expired lease certifies neither.

RAM preview exposes requested count, historical successful preparation count, live exact retained
indices, and the longest contiguous half-open frame-grid span separately. Ties choose the earliest
span. `Complete`, `PartialBudget`, `Cancelled`, and `Failed` describe the run's outcome; none freezes
live coverage after it ends. For example, preparing four frames into a three-frame cache can leave
indices `{1,2,3}`: four preparations, three retained samples, span `[1,4)`, and partial completion.
It does not certify frame zero or all four requested frames. Cancel/failure also preserves any
still-valid samples, without autoplay. Completion callbacks cannot autoplay a superseded run,
project, resolution or range.

Coverage is projected from retained entries on mutation/result/rebase boundaries, not by scanning
composition duration in a paint or getter. Cache-owned resident validity refresh runs every 250 ms
only while resident entries exist, reading atomic lease validity without crossing native ownership.
Expired entries are removed, and the existing 50 ms coalesced notification updates consumers;
worst-case idle coverage latency is approximately 300 ms plus event-loop scheduling. This interim
refresh costs one retained-entry scan per active interval, has no per-frame observer queue, retains
no extra leases, and is stopped when the resident set becomes empty. Direct membership and the
full-coverage autoplay boundary check lease validity immediately. Delivery cadence remains separate
work; this timer does not replace task completion delivery.

### Per-frame UI cost during playback (#358)

A playhead move is a value change, not a document change, and every surface treats it so:

- Properties refreshes the values and key indicators of its existing rows in place; it never
  rebuilds rows or re-walks the upstream graph on a time change (`PropertiesEditor::work()`).
- Driven values compile their probe once per document state, composition and parameter set; each
  later time only evaluates the cached plan's value graph (`CompositionSession::drivenValueWork()`).
- The Timeline lane region and the curve view invalidate only the strips the playhead leaves and
  enters. Collapsed layers' key summaries are cached per document state, and curve polylines per
  compiled-curve generation, time axis, size and value viewport. The ruler merges adjacent cached
  frames into one RAM-preview coverage segment.
- Audio remixes only when an edit moved the active composition's evaluation provenance
  (`CompositionSession::lastDocumentChangeAffectedEvaluation()`), keeps the previous mix audible
  until the new one is ready, and silences explicitly only on a composition switch or document
  rebind. Layout-only edits, edits the render reachability proves neutral, and edits to
  compositions this one does not render cause no remix and no dropout.

Each of these has work counters, and tests assert them over sixty playhead moves.

### Playback never blocks

Space and the transport button toggle the same session transport. Space belongs to the window and
works even when every Timeline panel is hidden or replaced; focused text entry keeps the character.
A gesture press pauses playback before its first time change. `Ctrl+Shift+Space` remains the explicit
RAM Preview command: prepare the range, then autoplay only when the entire current range remains
retained. Partial retention does not introduce a narrower transport range or change the work area.

Each due tick advances the exact session time and asks for its frame:

- A cache hit publishes immediately without evaluation or coalescing.
- A miss is admitted at `Visible` whenever no foreground request is active/pending. The measured
  delivery estimate does not gate an idle admission: it is only refreshed by a successful
  completion, so refusing to submit on an unknown or slow estimate would starve a cold, uncached
  range forever. Delivery timing includes queue, preparation and queued completion dispatch; Jobs
  progress polling is independent.
- Same-domain playback keeps one active producer and one newest pending intent. A busy miss
  replaces only that pending intent; newer ticks do not cancel useful active work. Admission remains
  blocked until the active handle reaches terminal state. Missed deadlines count lateness once,
  independently of whether preparation later succeeds.
- A clean, valid exact-identity older completion may enter the cache at its own rational time and
  display coherently as Stale while desired time/generation remains newest. It cannot overwrite a
  newer displayed or cached generation, including after a loop. Identity and generation guards are
  rechecked around cache/accounting callbacks, preventing purge or same-ID rebind from reviving an
  old result. Failed/unsupported late work cannot claim pixels and contributes at most one drop.
- Incompatible domain, edits, scrub, rebind, purge, pause and shutdown retain cancellation and
  terminal-drain barriers. Pause queues one normal Visible replacement unless a live Current frame
  already matches; shutdown closes admission before pause recovery can enqueue work.

Ordinary playback targets total elapsed steady time or authoritative audio position independently
of cache membership. A delayed 200 ms tick at 25 fps requests frame five for both warm and cold
ranges; repeating the same clock sample does not advance time. Cache hits affect delivery, not the
transport clock. Explicit RAM Preview still waits for complete retention before starting playback;
it does not introduce a slower one-frame-per-callback transport mode.

Wall and audio origins select the nearest valid work-area frame-grid sample, with exact halfway
ties choosing the greater index. Play preserves the immediate stored session time when it is inside
the authored work area. Ordinary nearest rounding is within half a frame; clamping to the first or
last valid grid sample may move farther, including inside an authored off-grid interval. Existing
outside-range behavior clamps session time to the first valid sample. First grid index is checked
at-or-after start; the last is strictly before the exclusive end. A range containing no grid sample
pauses playback and refuses a new play. Live frame-rate/range changes reanchor the current valid
grid and clocks; disabling audio reanchors the wall clock at current time. Checked arithmetic
refuses invalid/overflowing targets rather than publishing a time outside the valid grid.

The precise single-shot timer requests positive ceil-millisecond wakes capped at 16 ms. Actual OS
wake lateness is allowed; each tick derives its target from elapsed time without accumulating tick
debt. A frame's absolute accounting deadline is captured when its playback intent is created and
copied unchanged from pending to active, so delayed admission cannot renew its budget. Existing
bounded active/newest, own-time Stale publication and cancellation/retirement barriers remain.

This bounded cold-progress repair is locally qualified by a short real solid/text animation:
actual time 0.916667 is displayed Stale while transport requests 1.0 and remains playing; pause
converges at 2.83333. Seven CPU pixel oracles, coherent handles, real text drag/commit and exact undo
pass. It changes no clock/deadline/ticker interval and adds no prewarm or CPU scene substitution.
Shared preview completion notifications/refill are now implemented by the bounded G4A subscription
and UI drain described in [Task System](task-system.md#completion-driven-result-delivery-g4a).
Cache-independent rational transport and intent-time deadlines are now implemented as bounded G4B.
Required native Qt present hooks and remaining scheduling/stress qualification remain G4 work;
sustained render rate and drift remain G10.

### Audio Clock Master

When audio is enabled, a revision-valid `AudioMixDescription` has been published, and the audio
backend starts successfully, `AudioEngine::positionNow()` is the playback master. It is derived from
frames written by the backend callback, the rational play origin, the output rate and playback rate;
the UI transport maps that exact position through `FrameTimeMapping` and never advances audio from
its wall-clock timer. Pausing stops the engine, scrubbing seeks then pauses, and a composition or
revision change clears the old clips before a new mix is accepted.

Looping keeps audio and pictures on one period. The transport wraps pictures with an exact modulo
of the work area's frame count and hands the mixer the same frame-grid range,
[first valid frame, last valid frame + 1); the mixer wraps the audio inside that range on its own
thread while `positionNow()` stays unwrapped, so every pass is audible and repeated wraps never
drift under either master clock. Turning looping off mid-run keeps the passes already played: the
run finishes the pass it is in and pauses on the last frame, and the mixer keeps that pass's offset.
Turning it on mid-run, or changing the work area, updates the mixer's range in place or with the
work-area re-anchor described above ([Media I/O](media-io.md#preview-engine-and-synchronization)).

If there is no resolved audio clip, audio playback is disabled, or the backend cannot start, the
transport falls back to its existing wall-clock frame schedule. That fallback is explicit and keeps
RAM Preview's prefill and cache behavior unchanged. Mix publication is revision-guarded, so a
completed worker plan cannot load clips for a newer document revision.

The existing supported audio playback-rate ratio scales the exact remaining media-frame fraction
before one ceiling to wall nanoseconds for deadlines and wakes. Invalid speeds or unrepresentable
products/divisors/budgets refuse explicitly. This adds no speed mode or audio backend; existing
non-1× audio position remains nanosecond-quantized. NullBackend tests qualify silent clock authority,
2×/0.5× budgets of 20/80 ms at 25 fps and live 60 fps reanchor budgets of 8,333,334/33,333,334 ns;
they do not qualify a physical audio device or audible output.

A bounded real-clock gate runs five competing tiny CPU-reference sessions for 60 seconds at
24/25/30/60/30000÷1001 fps with zero exact desired-grid mismatches. Maximum observed wake gap is
17.809458 ms despite the requested 16 ms cap. Prepared products are measured separately and no
Viewer paints in that gate. Separate private seven-phase GPU pixels qualify short animated visible
progress, real text drag/commit and exact undo. These proofs establish clock/grid and image
correctness, not painted FPS, sustained GPU throughput or input-to-display latency.

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
unchanged; compiled plan and derived operation caches bypass overridden requests. The one read the
override path may still take is a warmed, already-verified native decoded still-image memory entry
(CACHE-1; see `media-io.md`'s "Read-only decoded-image exception"), never a derived operation
result. Preview controller and
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
or committed pixels. Override plans and frames remain excluded from revision caches. The evaluator's
memory cache admits one exception for interactive overrides: read-only reuse of an already-verified
decoded still-image source entry (CACHE-1); an explicit evaluation bypass
(`request.bypassOperationCache`) still turns even that off.

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

A layer samples its own position, anchor, scale and rotation at its [layer time](#layer-time), then composes
its local matrix with its parent's resolved matrix, which the parent samples at its own layer time
derived from the child's placement time. This continues through the ancestor chain.
Animated and driven parent values therefore move the child's evaluated content bounds, anchor and
viewer overlay polygon at the same time as its pixels. Cache dependencies include the resolved
parent transform; changing an ancestor cannot reuse stale child geometry or imagery. Parent opacity,
enabled/solo state and time range do not suppress or fade children. The child's own range and opacity
continue to apply independently.


## Path Curves

[ADR 0028](../decisions/0028-layer-masks-and-track-mattes.md) adds `PathAnimationCurve`, a
whole-value curve of `PathKeyframe { id, time, value: PathValue, outgoingInterpolation, ease
handles }`; only a mask path (`bloom.mask.path`) may own one. A sample before the first key or after
the last holds the end key. Inside a segment the factor is the scalar sampler's own 0 → 1 segment
with the keys' interpolation and handles (a handle's value offset is a fraction of that progress),
and each anchor's point and handles interpolate by it; a missing handle counts as its point. Keys
whose anchor counts or `closed` flags differ hold the earlier key. Path curves compile inline into
the mask operation that samples them and are sampled at the operation's own clock time. Keyframe
move, delete, interpolation and paste commands treat a Path key as a whole-value key; value and
handle edits that address a scalar axis do not apply to it.

## Layer Time

Status: accepted and implemented ([ADR 0024](../decisions/0024-layer-owned-time.md), issue #353).

The time mapping is layer-owned. A layer converts its placement time `t` (composition time for a
top-level layer, the enclosing layer's content time for a nested one) into two clocks:

```text
layer time    tau = (t - startTime) * speed
content time  c   = outOfExtent(timeRemap enabled ? exact(timeRemap sampled at tau) : tau)
```

- The activity window `[inPoint, outPoint)` is tested in placement time first, unchanged.
- The Layer Output's own parameters, including the Time Remap curve, sample at `tau`.
- Everything feeding the Layer Output's image and audio inputs evaluates at `c`: sources, effects,
  Merges, nested layers, composition sources, and the value nodes that drive those operations. A value
  node driving the layer's own parameters evaluates at `tau`.
- Moving `startTime` therefore moves every key of the layer and its upstream without rewriting a key.

`startTime` is exact and may be negative or beyond the duration; `speed` is an exact positive
rational in `[1/1000, 1000]`. `contentExtent` is `Unbounded`, `Fixed{duration, frameRate}` or
`Source{nodeId}`, the last resolved at compile time from the source's media or composition length.
`beforeMode` and `afterMode` are the closed enumeration None / Hold / Loop / PingPong and are inert for
an `Unbounded` extent. With extent duration `E` and last frame start `L`:

| Region | None | Hold | Loop | PingPong |
| --- | --- | --- | --- | --- |
| `r < 0` | absent | `0` | `r mod E` (non-negative) | reflect, period `2L` |
| `r > L` (Hold) / `r >= E` (others) | absent | `L` | `r mod E` | reflect, period `2L` |

Absent content behaves like a time outside the activity window: no image slot and silence. The
mapping is one shared core in `src/runtime/composition_time.cpp`; `mapCompositionTime` is that core
with `before = Hold` and `after = loopMode` and keeps its current results bit-for-bit. Arithmetic is
checked rational with the existing nanosecond fallback and `ArithmeticOverflow` diagnostic.

Source-level timing composes inside the layer: `source time = sourceMapping(c)`. Image, video and
audio `startFrame`/`loopMode`, and composition-source `timeOffset`/`timeScale`/`loopMode`, are not
migrated; they remain the only timing for a source consumed without a layer. Layer-creating commands
leave the source at identity and put timing on the layer.

The compiler places operations in time scopes. A layer with identity timing creates no scope, so a
document opened from an earlier schema compiles to the same operation list. A non-identity layer
creates a scope whose parent is its placement scope; an operation reached from several scopes is
compiled once per scope and keeps its source-node identity, so the operation cache shares invariant
copies and time-dependent copies that map to the same content time. Mapped time is the cache
identity for time-dependent work. Preflight resolves scope clocks top-down and samples each distinct
time once. Scope and post-clone operation counts are capped with
`bloom.runtime.compile.time-scope-limit`. Plan semantics advances to 9; animation, evaluator and
primitive versions and all pixel goldens are unchanged.

GPU scene preparation walks the same scopes as host parameter resolution; no pixel operation is
added. The coverage contract adds Required axes `feature.layer.time.offset_speed`,
`feature.layer.time.remap`, `feature.layer.time.extent_modes` and `feature.layer.time.shared_upstream`
with CPU parity on every required preview and final route.

Audio uses the same mapping per output sample, as a layer entry ahead of nested-composition entries
in the clip's ordered mapping list (`AudioCompositionTimeMapping::layerTime`); the clip keeps the
compiled plan so a curve Time Remap is sampled at each sample's layer time. Speed and remap are
varispeed and None is silence. A driven (value-graph) Time Remap on an audio layer is resolved once
per mix at the request time, and such a mix is re-evaluated with the transport like an animated
level.

Audio timing (#354):

- Every layer clip carries its own layer's range entry, including identity timing, so a trimmed
  head is silent before the in point and the out point ends the clip; a source wired straight to
  the output gates on the composition range.
- A video source's embedded audio follows its picture's `loopMode`: Loop wraps the source-local
  time (`t - startFrame`) at the picture extent (frame count times the stream period) and PingPong
  reflects it about the last frame start, exactly as the picture's frame index wraps. The wrap is
  applied after every layer and composition mapping (`wrapSourceAudioTime`). Before its start frame
  the stream is silent, and Hold plays the stream once: a held picture frame has no sound. An audio
  source has no loop mode.
- Layer modes, speed, remap and nested compositions map audio exactly as pictures (Hold repeats the
  held sample, as for a nested composition). Export re-evaluates the mix per block and playback
  per transport change through the same `mapAudioClipTime` and mixer, so they produce identical
  samples; the per-sample mapping allocates nothing on the mixer worker.

Implementation notes:

- A Time Remap result within one nanosecond of a frame start of the extent's frame rate (the
  composition rate without an extent) is snapped onto that frame start, so binary64 remap seconds
  that mean frame n (an identity remap at 1/24 s steps) never select frame n - 1.
- An operation copied onto several clocks shares its parameter and curve identities; preflight
  claims them once, for the first copy. The Time Remap operand is claimed with the Layer Output.
- A `Source` extent resolves through `runtime::resolveSourceExtent` (`layer_time_projection`): a
  numbered sequence, a video, an audio asset, or a composition source with constant identity offset
  and scale; anything else compiles `Unbounded` with `bloom.runtime.compile.layer-extent-unresolved`.
- `AddNode` never binds Time Remap (absence means disabled); `EnableLayerTimeRemap` binds it with two
  Linear keys mapping the layer range identically, and `DisableLayerTimeRemap` erases it with its
  curve.
- Key-edit footprints project layer-clock keys (and upstream keys where content time equals layer
  time) through the layer's start and speed into its span; remapped or wrapping upstream keys cover
  the layer span, and a retimed layer nested inside another layer's upstream stays whole-composition.
- The coverage contract's Required axes `feature.layer.time.offset_speed`, `.remap`,
  `.extent_modes` and `.shared_upstream` are prepared by the production builder and executed with CPU
  parity on the native gate.

A Qt-free projection gives the Timeline the forward map, the exact inverse
`t = startTime + tau / speed` for layer-clock keys, and the media extent in placement time for the
bar. Upstream keys project through the same linear inverse and are marked as content time while a
remap or wrapping mode is active. Keyframe insertion at the playhead converts the session time
forward to the parameter's clock and is refused where content is absent. Edit footprints are listed
in [ADR 0024](../decisions/0024-layer-owned-time.md#8-projection-and-footprints).

## Composition Settings Changes

`SetCompositionSettings` (the Composition Settings dialog, and `bloom.composition.set-settings`
through the operation registry) replaces a composition's name, format, duration, timecode,
background, working colour space and safe areas as one undoable command. Its render footprint is the
whole composition at every time, which also invalidates every composition that nests it. Settings
equal to the current ones commit nothing, and an invalid value is refused with a plain reason before
anything changes. Under [ADR 0024](../decisions/0024-layer-owned-time.md) time belongs to layers and
is exact, so the edit rewrites no time:

- **Frame rate.** Layer `startTime`, `inPoint`, `outPoint`, speed, remap, and every key keep their
  exact rational seconds. A key that sat on a frame of the old rate keeps its time, so at the new
  rate it may fall between frames; it is sampled where the new frame grid meets the curve, exactly
  as any off-frame key is, and is never snapped or moved. Frame snapping in later gestures uses the
  new rate. A nested composition is sampled at its own new rate by the mapping below, with the
  parent's layers unchanged.
- **Duration.** Layer and key times are unchanged; anything past the new end simply lies outside
  `[0, duration)` and comes back if the duration grows again. A work area is clamped to the new
  duration and cleared if nothing of it remains.
- **Start timecode and drop-frame** change only labels (see the project format's composition
  timecode); frames and times are untouched.
- **Size.** The 3 × 3 resize anchor decides where existing layers sit: each unlocked, unparented
  layer's position moves by `(newWidth - oldWidth) * column / 2` and
  `(newHeight - oldHeight) * row / 2` (column and row 0, 1, 2 from top left), so centre keeps layers
  centred and top left keeps them at the same offset from that corner. The shift is applied to the
  constant, to every key of an animated position, and to its default; a driven position is left to
  its driver. Parented layers follow their parent, and locked layers stay where they are, as every
  command leaves locked layers untouched. Undo restores every moved value exactly.

## Composition Source Time Mapping

A composition source samples its offset and scale at its scope's content time (the containing
composition's time when no retimed layer encloses it), then uses
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
