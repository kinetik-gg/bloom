# Animation, Time, And Interaction

Status: accepted

Updated: 2026-08-25

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

## Durable Type Model

`KeyframeId` is a project-global strong ID with allocator and high-water semantics identical to the
other durable typed namespaces. A key keeps its identity when its time or value changes, and deleted
IDs are never reused.

A composition owns an `AnimationCurveStore` containing records ordered canonically by
`AnimationCurveId`:

```text
KeyframeInterpolation = Hold | Linear

ScalarKeyframe = { KeyframeId, RationalTime, Float64, outgoing interpolation }
Vec2Keyframe   = { KeyframeId, RationalTime, Vec2d,   outgoing interpolation }

AnimationCurveRecord = ScalarCurve | Vec2Curve
```

Each curve contains at least one key. Keys are stored in strictly increasing exact rational time;
two keys at the same normalized time are invalid. Values are finite. Opacity curve values are also
within `[0, 1]`. The final key's outgoing interpolation is always normalized to `Linear` on every
mutation, keeping equality, plan identity, and persistence canonical. It becomes a Linear segment
if a later key is inserted.

Version 1 curve ownership is deliberately narrow:

- every animation-curve parameter source resolves to one curve in the same composition
- the curve value kind matches the parameter schema exactly
- every curve is referenced by exactly one parameter
- position accepts a `Vec2Curve`; opacity accepts a `ScalarCurve`
- Solid color and text schemas declare animation unsupported
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
5. For an interior `Linear` segment, compute one shared factor
   `(time - left) / (right - left)` and apply Float64 scalar Mix version 1 to the scalar or each
   `Vec2d` component.

The interval factor is derived as an exact non-negative rational. Products and differences of valid
signed 64-bit rational components require at most 256-bit unsigned magnitude. Bloom therefore uses
a private fixed-width multiword `UInt256`, normalizes the quotient, and rounds directly to binary64
with round-to-nearest, ties-to-even. Exact endpoints produce exact `0.0` and `1.0`. The algorithm
does not route through seconds, `long double`, compiler-specific extended integers, or `libm`.
Overflow, an invalid denominator, an unsupported floating-point environment, or a non-finite mixed
result is a structured evaluation failure.

`kAnimationSamplingSemanticsVersion` is `1`. It enters compiled-plan compatibility and cache
identity alongside scalar, evaluator, and image primitive versions.

## Compiled Plan And Evaluation

Runtime uses distinct typed operands rather than a per-pixel variant lookup:

```text
CompiledScalarParameter = { ParameterId, Float64 | ScalarCurveIndex }
CompiledVec2Parameter   = { ParameterId, Vec2d   | Vec2CurveIndex }
```

An immutable plan owns separate scalar and `Vec2d` curve tables ordered by `AnimationCurveId` and
contains only reachable curves. Layer Output operations reference the compiled position and opacity
operands; Solid color remains constant. Indices are strong types so the wrong table cannot be
addressed accidentally.

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

Converting a constant parameter to animation seeds its first key with the current constant value.
Converting animation back to a constant is an explicit transaction that chooses the constant and
deletes the now-orphaned curve atomically. Editing a driven parameter requires an explicit driver
transition; a gesture never disconnects it silently. One transaction produces one undo entry and
restores exact curve/key records, IDs, sources, times, and values on undo/redo while preserving the
maximum live allocator high-water; history never rewinds an allocator counter. Allocation
exhaustion is a structured command failure and rejected commands do not consume IDs.

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
immediately advances the desired request generation. Scrub, playback, and direct manipulation use
`Interactive` priority; discrete typed time entry, key selection, and document refresh use
`Visible`. The controller uses an injectable 16 ms trailing cadence for pointer storms and retains
at most one active request handle plus one newest pending request per preview owner. A superseded
active request is cancelled but remains active until terminal; only then may the pending request be
submitted. Scrub end bypasses the trailing delay but does not violate that active-request gate.
Scheduler coalescing and stale-result rejection remain lower-level backstops.

Keyframe selection stores the stable `KeyframeId`; row index and screen position are presentation
details.

## Direct Manipulation And Preview Overrides

An active position interaction is session-only state:

```text
PositionInteraction = {
  base document revision,
  target ParameterId and LayerId,
  exact current time,
  base Vec2d value,
  current Vec2d override
}
```

Gesture begin freezes a non-empty mapping rectangle, composition format, proxy, pixel aspect, and
display descriptor for the current composition. A missing current-composition mapping rejects the
gesture. Resize, DPI, format, proxy, pixel-aspect, or display-descriptor changes cancel it; a stale
frame from another composition is never used as a mapping source.

Pointer motion derives the override from the base value plus total gesture displacement, not from a
chain of already-rounded intermediate positions. Given the frozen fitted composition rectangle:

```text
compositionDx = screenDx / displayWidth  * compositionWidth
compositionDy = screenDy / displayHeight * compositionHeight
```

The display rectangle already accounts for proxy scaling and pixel aspect. Positive X is right and
positive Y is down, matching the evaluator's position contract.

`SnapshotCompileRequest` carries zero or one typed parameter override in version 1. Admission checks
captured revision, target existence, reachability, schema/value kind/domain, then source kind, in
that order. Constant and compatible animation sources accept the override; `DriverBinding` rejects
it explicitly. Compilation lowers an accepted override as a constant only for that request and
never hides or disconnects a driver. The complete override value and target enter deep plan/cache
identity.

On release, a constant position receives one set-constant transaction. An animated position updates
the exact-time key or inserts one. A zero move commits nothing. Escape, secondary-button cancel,
capture loss, a stale revision, a missing target, a parameter-source change, a composition switch,
or a frozen-mapping change clears the override and creates no command. Version 1 has no durable
layer lock field, so all otherwise valid targets are treated as unlocked. It supports one active
translation interaction; locking, multi-selection transforms, and constraint modes are deferred.

## Required Verification

- exact ordering and interval selection at extreme normalized rationals
- binary64 factor rounding at extreme representable ratios and halfway ties; subnormal key
  values/Mix results are covered separately because signed-64-bit time components cannot produce a
  binary64-subnormal interior factor
- Hold/Linear endpoints, extrapolation clamps, scalar/Vec2 sampling, and non-finite mix failure
- curve/source/kind/ownership validation and project-global curve/key ID collisions
- create/insert/update/delete/source-transition undo and redo with exact identities
- snapshot/plan equality and cache identity for curve, time, and override changes
- one sample per referenced curve per request and no row-loop allocation
- bounded scrub storms, stale-frame suppression, scrub-end flush, and session-only time
- commit, cancel, capture loss, mapping change, stale target, driven target, and zero-delta gesture
  behavior

Bezier curves, tangents, curve modifiers, procedural extrapolation, expression sampling, shared
curves, playback audio sync, multi-layer transform gestures, and onion skinning are explicitly
deferred.
