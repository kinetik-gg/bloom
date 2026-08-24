# ADR 0016: Typed Animation Curves And Exact Rational Sampling

Status: accepted

Date: 2026-08-25

## Context

Bloom's layer, node, Properties, timeline, scripting, and evaluation surfaces must address the same
animated parameter without baking values or inventing editor-specific time semantics. Frame labels
are insufficient project truth: compositions may use rational rates, keys may lie at subframes or
outside the visible range, and CPU and future GPU paths must not select different intervals because
of floating-point time comparisons.

The first animation slice needs scalar opacity and two-dimensional position. Cubic curves would
also require durable tangent, weighting, editing, extrapolation, serialization, and conformance
contracts that are not needed to prove native animation.

## Decision

- Add project-global `KeyframeId` values and composition-owned, strongly typed scalar and `Vec2d`
  animation curves. Curve and keyframe declarations participate in the same durable allocator
  high-water and validation rules as other document objects.
- A parameter source references a compatible curve in the same composition. Batch 4 permits exactly
  one parameter per curve; shared curves and linked animation remain deferred rather than receiving
  accidental copy-on-write behavior.
- Store key times as normalized `RationalTime` and order/search them with exact rational comparison.
  Conversion to seconds is never used to choose a key or interval.
- Version 1 supports outgoing `Hold` and `Linear` interpolation. Values before the first key and
  after the last key clamp to the endpoint; an exact key returns its stored value exactly.
- Linear interpolation derives the exact rational interval factor with a portable fixed-width
  multiword integer implementation and rounds it once to binary64 using round-to-nearest,
  ties-to-even. Evaluation then uses the versioned Float64 scalar Mix primitive per component.
- Compile reachable constant-or-animated parameters to strongly typed parameter operands and
  immutable, typed curve tables. Sample each referenced curve once per evaluation request before
  image-row work begins.
- Declare animation capability in the parameter schema. Version 1 animates Layer Output position
  and opacity; Solid color and text remain constant until their own semantics are accepted.
- Assign animation sampling semantics version `1` and include it in plan and cache compatibility.
  Changing interval selection, extrapolation, rounding, or interpolation requires a new version.

The implementation contract is maintained in
[`../architecture/animation-and-time.md`](../architecture/animation-and-time.md).

## Consequences

- Timeline, structured properties, graph views, and Python can mutate one stable animation model.
- Sampling is deterministic at extreme valid rational times and does not depend on `long double`,
  compiler-specific `__int128`, platform `libm`, or frame-rate conversion.
- The first UI may frame-snap playhead gestures without destroying exact subframe key times.
- Cubic interpolation, tangent editing, expression/driver evaluation, curve sharing, modifiers,
  cycles, and procedural extrapolation require later explicit contracts.
