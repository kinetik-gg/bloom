# Animation, Time, And Interaction

Status: accepted

Updated: 2026-09-13

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
KeyframeInterpolation = Hold | Linear | EaseInOut

ScalarKeyframe = { KeyframeId, RationalTime, Float64,  outgoing interpolation }
Vec2Keyframe   = { KeyframeId, RationalTime, Vec2d,    outgoing interpolation }
Color4Keyframe = { KeyframeId, RationalTime, Color4d,  outgoing interpolation }

AnimationCurveRecord = ScalarCurve | Vec2Curve | Color4Curve
```

Each curve contains at least one key. Keys are stored in strictly increasing exact rational time;
two keys at the same normalized time are invalid. Values are finite. The final key's outgoing
interpolation is always normalized to `Linear` on every mutation, keeping equality, plan identity,
and persistence canonical. It becomes a Linear segment if a later key is inserted; the final key is
therefore the one key whose interpolation cannot be chosen, and asking for anything else there is
refused rather than silently normalized.

A `Color4Curve` carries straight/unassociated authoring RGBA, the same encoding a constant solid or
text color already uses. A color key is valid exactly when `core::Color4d::isValid()` accepts it:
finite RGB -- negative and HDR channels included -- and alpha within `[0, 1]`. Animating a color
therefore cannot reach a value a constant one could not.

Curve ownership is deliberately narrow:

- every animation-curve parameter source resolves to one curve in the same composition
- the curve value kind matches the parameter schema exactly
- every curve is referenced by exactly one parameter
- position, anchor, and scale accept a `Vec2Curve`; rotation, opacity, and text size accept a
  `ScalarCurve`; solid color and text color accept a `Color4Curve`
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
5. For an interior `Linear` segment, compute one shared factor
   `(time - left) / (right - left)` and apply Float64 scalar Mix version 1 to the scalar, to each
   `Vec2d` component, or to each of the four `Color4d` channels.
6. For an interior `EaseInOut` segment, transform that same shared factor before mixing, exactly as
   described below.

The interval factor is derived as an exact non-negative rational. Products and differences of valid
signed 64-bit rational components require at most 256-bit unsigned magnitude. Bloom therefore uses
a private fixed-width multiword `UInt256`, normalizes the quotient, and rounds directly to binary64
with round-to-nearest, ties-to-even. Exact endpoints produce exact `0.0` and `1.0`. The algorithm
does not route through seconds, `long double`, compiler-specific extended integers, or `libm`.
Overflow, an invalid denominator, an unsupported floating-point environment, or a non-finite mixed
result is a structured evaluation failure.

### Ease In-Out

`EaseInOut` is a cubic Bezier ease with FIXED symmetric handles at `(1/3, 0)` and `(2/3, 1)`. Those
x handles are what make it exact and allocation-free: a cubic Bezier whose x control points are
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

`kCompiledCompositionPlanSemanticsVersion` is `2`. It moved from `1` when those three source fields
stopped being resolved constants: a version-1 plan's `color` field was a `Color4d`, while a
version-2 plan's is a parameter that may index a curve table, so the same field position means
something different.

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

### The Keyframe Gesture

Clicking a parameter row's keyframe diamond, at the session's current time, is the one gesture that
creates and removes animation. It has exactly four transitions, each ONE transaction and therefore
one undo step:

1. A CONSTANT parameter becomes animated with one key at the current value and time --
   `CreateAnimationForParameter` and `SetKeyframeAtTimeForParameter` together in one transaction.
2. An ANIMATED parameter with no key at the current time gains one, valued at the curve's own exactly
   sampled value there, so inserting a key never moves the picture.
3. An ANIMATED parameter WITH a key at the current time loses it.
4. Losing the curve's LAST key converts the parameter back to a constant holding that key's value,
   erasing the orphaned curve atomically.

The diamond paints three states, read from the document on every refresh and never cached: empty for
a constant parameter, outlined for an animated one with no key at the current time, filled for one
with a key there. A parameter this gesture cannot key -- an unanimatable schema, or a driven source --
shows no diamond at all, because an inert affordance is a worse lie than an absent one.

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
and export resolution remain unchanged.

### RAM Preview

Preview frames are kept in memory so that playing a range a second time, or stepping back to a frame
already rendered, costs a lookup rather than an evaluation.

**Cache key.** Everything that decides a frame's PIXELS and nothing else: project, composition,
document revision, exact rational time, preview output, resolution (which is where a proxy factor
lives), resolution policy, quality, and color intent. That is `PreviewRequestIdentity` minus its request generation,
because the generation says which ASK a frame answered, not what it contains -- a hit is therefore
re-stamped with the asking request's own generation before it is published, so the frame the artist
sees is the answer to the request they made.

The display identity is a cache-wide tag rather than a key field: the qualified display processor
publishes once per session, so an entry's display identity can change at most once, and when it does
every earlier entry is stale. A frame whose qualification differs from the tag clears the cache and
adopts the new one.

**What is retained.** The packed RGBA8 display buffer and the identity, and NOT the Float32 process
image it was mapped from. Playback paints the packed buffer and nothing else -- the viewer's own
painting, its display geometry, its colour-state chip, and the direct-manipulation mapping all read
that buffer, none of them the process image -- so keeping the process image would spend four fifths
of the budget on pixels nothing in a preview ever reads. A retained frame is therefore about 8 MB at
1920x1080 rather than about 41 MB.

Anything that DOES need scene-linear pixels -- a frame or sequence export, a future sampler or
analysis -- evaluates the frame again rather than being handed a cached one, and asks
`PreparedPreviewFrame::hasProcessFrame()` rather than assuming. That is the one thing a cache hit
cannot answer, and it is stated rather than papered over with a silently null handle.

**Budget.** `playback/ram-preview-memory-bytes` in QSettings, 2 GiB by default; missing, unparseable,
or zero reads as the default. Least-recently-used entries are evicted until the budget is satisfied,
and a frame larger than the whole budget is refused rather than allowed to evict everything for
itself. At about 8 MB a frame the default budget holds roughly 250 frames of a 1920x1080
composition -- ten seconds at 24 fps -- and a RAM preview whose range does not fit stops at the
first eviction and keeps the prefix that does.

**Invalidation is the key.** A document edit advances the revision, so every entry of an earlier
revision is unreachable by construction; those entries are dropped outright when a frame of a newer
revision arrives. Two requests never reach the cache at all: one carrying an interactive parameter
override, whose pixels belong to a gesture rather than to the revision and whose identity cannot say
so, and an explicit refresh, which asks for the frame to be re-derived precisely because something
the key does not cover may have changed.

**The RAM Preview command** (`Ctrl+Shift+Space`, the Composition menu, and the Timeline transport's
own button) pre-renders the composition's whole frame range into the cache one frame at a time, in
the background, reporting "Caching 42/240" in the Viewer footer and cancellable with Escape, then
asks the transport to play it. Frames already cached are counted without being rendered again, so a
second RAM preview of an unedited range is immediate. The range is the composition's own
`[0, duration)`: Bloom has no work-area range to scope it to, since the timeline's work-area strip
honestly spans the whole duration and the document model has no in/out points.

**Clock rule.** A tick asks whether the NEXT frame is cached.

- Cached: the target advances by exactly ONE frame -- a cached frame costs a lookup, so there is
  nothing to drop and skipping one would state something about the composition that is not true. The
  due moment is still total elapsed time since `play()`, so presentations track the ideal frame grid
  and no per-tick error accumulates. A host that stalls long enough to owe several frames plays every
  one of them, at one frame per tick, rather than skipping to the frame the wall clock now demands.
- Not cached: the elapsed-time policy below is unchanged, and the footer keeps reporting what the
  coalescing preview path dropped.

A cached request never enters the coalescing path at all -- it is published directly, with no task --
which is why a fully cached playback run reports zero dropped frames rather than a small number.

### Dropped-Frame Counter

The Viewer footer reports "N dropped" while a playback run is in progress. It counts exactly the
requests the coalescing preview path discarded: a pending request superseded before submission (under
either the active-task gate or the Interactive trailing-cadence window), a pending Interactive request
discarded by a Visible bypass, and a finished frame thrown away unpublished because a newer pending
request exists.

It is deliberately a COUNT, not a rate, and it makes no real-time claim. The transport's own decision
to skip frame indices -- it recomputes its target from total elapsed time, so a slow evaluation makes
the next tick jump rather than slow the motion down -- never reaches the preview controller as a
request at all and is therefore not counted here. Counting is armed and reset by `play()` and disarmed
by `pause()`, so the figure shown always belongs to the run in progress; outside a run the footer says
nothing rather than a stale or invented number, and during one it reports zero explicitly, because
silence would read as "not measured".

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
- Hold/Linear/EaseInOut endpoints, extrapolation clamps, scalar/Vec2/Color4 sampling, the eased
  factor at the interval's exact thirds and midpoint, and non-finite mix failure
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

Per-key Bezier tangents (handles an artist can drag -- `EaseInOut`'s handles are fixed), curve
modifiers, procedural extrapolation, expression sampling, shared curves, playback audio sync,
multi-layer transform gestures, and onion skinning are explicitly deferred.
