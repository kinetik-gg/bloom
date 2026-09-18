# ADR 0023: Instances and per-element data

Status: proposed

Date: 2026-09-17

## Context

Procedural motion design is built on repetition with variation: a hundred copies of one shape laid
out on a grid, on a ring, or along a path, each carrying its own offset, rotation, colour and delay.
Bloom has no way to express that today. An artist who wants it duplicates layers, and a hundred
duplicated layers is a hundred timeline rows, a hundred undo targets, a hundred things to re-edit
when the design changes, and a document whose size is proportional to a number the artist thinks of
as one parameter.

[`node-catalogue.md`](../product/node-catalogue.md) reserves `Repeater` and `Scatter` at level `M1`
with the phrase that states the requirement exactly: *instance-like repetition without eagerly
duplicating authoring data*. This record decides what that means in Bloom's document, plan,
evaluator, cache and UI, and what it deliberately does not mean in its first version.

The question underneath is whether instancing needs anything new at all. Bloom already has a shared
upstream source, a Layer Output that transforms it, and a Merge that composites an ordered list. N
instances could be N Layer Outputs over one Solid feeding one Merge. That would be no new plan
grammar, no new evaluator stage, and no new cache rule. Task INSTANCE-0 built that plan by hand and
measured it before deciding anything else.

## Measured baseline

`tools/spikes/instance0/` compiles the expanded plan -- one Solid, N Layer Outputs over it in a
grid, one Merge holding all N, one Composition Output -- and evaluates it through the real
`CpuCompositionEvaluator`. Release build, clang 22.1.8, x86-64, 1920x1080 composition, 64x64
instance cells, single-threaded (no row-band pool), reference quality:

| Measurement | N = 500 | N = 2000 |
| --- | --- | --- |
| Preflight estimate against a 2 GiB interactive budget | **refused**, `requiredPeakBytes=16,621,977,600` | **refused**, `requiredPeakBytes=66,388,377,600` |
| Bytes actually retained after the frame | 97 MiB | 292 MiB |
| Cold evaluation | 49.8 - 52.1 ms, 503 misses | 185.4 ms, 2003 misses |
| Second evaluation of the same plan | 1.09 - 1.31 ms, 503 hits, 0 misses | 5.66 ms, 2003 hits, 0 misses |
| One instance moved, plan republished at a new revision | 18.0 - 18.4 ms, 500 hits, 3 misses | 60.8 ms, 2000 hits, 3 misses |

Cost is linear in N across the sweep (1, 50, 100, 250, 500, 1000 instances): about **0.093 - 0.10 ms
and 134 KiB retained per instance**. A 16.7 ms interactive frame therefore holds roughly 170
instances at this cell size, and 4 K cells would hold far fewer.

Four findings decided this record:

1. **The expansion is refused before it draws a pixel.** The evaluator's preflight charges one FULL
   composition-sized image for every image operation alive at once
   (`cpu_composition_evaluator.cpp`, the `peakBytes` loop). N instances feeding one Merge are all
   alive at once, so the estimate is N frames: 15.5 GiB at 500 instances and 61.8 GiB at 2000,
   against a 2 GiB interactive budget and against 97 MiB and 292 MiB actually used. The estimate is
   conservative by design and correct for hand-placed layers; it is wrong by two orders of magnitude
   for an instance set, and no budget an artist's machine can offer would make it right.
2. **The operation cache already serves an unchanged instance set from digest.** A second evaluation
   of the same plan is 503 hits and zero misses at 46x the speed. Instancing does not need a new
   cache; it needs to be ONE entry instead of N.
3. **Content addressing already gives per-instance invalidation -- and it is not enough.** Moving one
   instance at a new revision re-uses 500 of 503 operations, yet still costs 18 ms at N = 500 and
   61 ms at N = 2000, because the Merge re-folds every entry. The cost of an edit is a function of
   the whole set, not of what changed. A dedicated composite stage can do better later; nothing in
   the Merge model can.
4. **Per-element attributes are cheap in time and expensive in structure.** 2000 per-element scalars
   evaluated as individual value-graph nodes cost 0.72 ms per frame -- affordable -- but add 2000
   plan nodes and 2000 operation-cache entries per frame. The reason to give the value graph array
   values is not arithmetic speed; it is that one attribute should be one node.

## Decision

Adopt an **Instancer**: one node, one compiled operation, one cache entry, one timeline row.

1. **`bloom.instancer` is an image operation, not a layer factory.** It takes one image input -- the
   PROTOTYPE -- and produces one image. It creates no `LayerId`, no stack slot and no timeline row.
   Its output feeds a Layer Output, a Merge or another operation exactly as any image does, so the
   layer model, the timeline and the selection model are unchanged by instancing existing at all.
2. **The prototype is evaluated once.** The instancer resamples and composites that one evaluated
   image N times through the existing affine resample and blend kernels. This is what makes the
   instancer cheaper than the measured baseline rather than merely tidier than it: the baseline
   re-evaluates nothing, but it does allocate, key, cache and re-fold N separate operations.
3. **Instances are derived state, never document truth.** The document stores the instancer's
   attribute BINDINGS. The N resolved elements exist only inside an evaluated frame, beside
   `EvaluatedOperationBounds`, and are readable through the same derived-state channel the viewer
   already uses for bounds. Nothing persists per element.
4. **An instance is not selectable and has no parameters of its own.** Clicking an instance selects
   the INSTANCER. The viewer draws the instancer's gizmo plus read-only ghost outlines for a capped
   number of elements. An artist edits the rule, not a copy of the result.
5. **Per-element attributes are a closed v0 set** -- `index`, `count`, `position`, `rotation`,
   `scale`, `opacity`, `colour`, `timeOffset`, `blendMode` -- plus arbitrary NAMED typed attributes
   that a solver may read and route into one of those. An attribute cannot reach a parameter of the
   prototype's own schema in v0; that is per-instance evaluation, which is the eager expansion this
   record exists to avoid.
6. **Attribute sources are: a data-block column, an array expression, a sampled curve or ramp, or
   seeded random.** The seed policy is frozen as `hash(instancerSeed, elementIndex, attributeKey)`,
   so adding or removing elements never reshuffles the ones that remain, and a cached or exported
   frame agrees with the frame that produced it. There is no entropy source, matching the existing
   `Random` value node.
7. **Layout and constraints are pure value nodes over ARRAY values.** The value graph gains an array
   value kind whose length is the instancing scope's count, an explicit compiled `Broadcast` for a
   scalar feeding an array operand, and a named `Array Reduce` for the other direction. Grid,
   radial, path-follow and spacing relaxation are ordinary value nodes, deterministic by
   construction: fixed iteration counts, index-order traversal, no convergence early-exit, no wall
   clock. The shapes, the scope rule and the read rule are in
   [`instancing.md`](../architecture/instancing.md).
8. **Instances composite in index order, index 0 topmost**, matching Merge's entry-zero rule so
   Bloom has one ordering convention. All elements use the instancer's own blend mode unless an
   explicit `Instance Merge` supplies a per-element one.
9. **One cache entry, keyed by an instance-set digest.** The instancer's operation key carries the
   existing preamble, the prototype's content hash, and a SHA-256 over the resolved attribute
   arrays encoded with the existing `OperationKey` byte rules. An unchanged instance set is one
   lookup. Editing one element still invalidates the whole instancer image in v0 -- which the
   baseline measurement shows is not a regression, since re-folding a Merge after a one-element edit
   already costs `O(N)`.
10. **Preflight charges three frames, not N**: the prototype, one reused per-element scratch buffer,
    and the instancer's own output. This is the direct repair of finding 1, and it is the reason the
    instancer is one operation rather than a compiler expansion into N.
11. **Bounds are the exact union of the transformed element rectangles**, computed from N
    rectangles rather than N images. STORAGE is that union intersected with the composition display
    window grown by a documented margin; content beyond the margin is clipped with a warning
    diagnostic. This is the one place instancing deliberately departs from Merge, which retains its
    intermediate outside the composition: a hand-placed layer's off-frame content is bounded by the
    artist's hand, and a generator's is not.
12. **Per-element time offsets apply only to a Composition Source prototype in v0.** Nested plans
    already evaluate at a mapped time and already key their cache by it. A non-zero time-offset
    attribute on any other prototype is a compile diagnostic rather than a silently ignored value.
    Distinct offsets cost one nested evaluation per DISTINCT mapped time, deduplicated.
13. **The scripting surface is the ordinary one.** Instancer authoring is typed commands in the
    existing registry, so ADR 0022's facade, `bloom-cli`, Python and MCP receive it without a
    bespoke path. The facade exposes the declared bindings and the resolved count as data, and the
    resolved per-element arrays only as a derived-state query on an evaluated frame. No client code
    runs per element, and none runs inside the evaluator.
14. **INSTANCE-1 moves no semantics version and re-derives no golden**, provided `CompiledInstancer`
    is APPENDED to the `CompiledOperation` variant, the array alternative is APPENDED to
    `CompiledValue`, no existing field changes meaning, and the composite reuses
    `layerTransformBilinearRow()` and `blendLinearRec709SceneRow()` unchanged. A new alternative
    appearing is explicitly not a plan-semantics change (`compiled_plan.hpp`), and a stage no
    existing plan can reach changes no existing frame. If any of those three conditions is broken,
    the lane bumps the affected version and re-derives through `s5-identity-oracle.py`, reproducing
    the current plan 7 / animation 2 / evaluator 8 / primitives 7 pins first.
15. **No document schema change.** A new node type stores only its type id, and an attribute binding
    is the existing durable `DriverBinding` record on an array-typed operand. Array operands
    therefore have NO authored constant -- they are driven or they fall back to their scalar default
    -- because a stored array would be the schema step this decision is avoiding.

### Limits

| Limit | v0 value | Why |
| --- | --- | --- |
| Instance count | 10,000 per instancer, refused above | At the measured 0.093 ms per instance an expanded frame is already ~0.9 s; the instancer is faster but not unboundedly so, and a cap the compiler states beats a frame that never finishes |
| Nested instancers | Allowed; the PRODUCT of the counts obeys the same cap | It falls out of the graph, so refusing it would be a special case, but the product is what costs |
| Bound attributes | 32 per instancer | Beyond it the binding UI stops being readable before the evaluator stops being fast |
| Attribute array storage | 4 MiB per instancer, checked at compile | 10,000 elements x 32 attributes x 8 bytes is 2.4 MB; the cap leaves headroom without admitting an unbounded table |
| Storage bounds margin | Composition display window grown by one composition extent on each axis | A parent transform can still pull off-frame instances back; an unbounded generator cannot |
| Drawn instance ghosts | 200, with a "showing 200 of N" readout | An overlay that draws 10,000 outlines stops being an overlay |

### Out of v0

Physics and simulation; per-instance materials or per-instance overrides of the prototype's own
parameters; GPU evaluation; per-instance selection, timeline rows, keyframes or masks; motion blur
derived from instance motion; per-instance dirty-region compositing; instanced audio; per-element
time offsets on a prototype that is not a Composition Source; 3D placement.

These are deferrals with a named owner in the implementation plan, not permission for the v0
boundaries to make them impossible.

## Consequences

An artist gets repetition as one node with one row, and a design change is one edit rather than N.
The timeline, the selection model, the undo history and the document size stop growing with a number
the artist thinks of as a parameter.

Bloom gains one compiled operation, one evaluator stage, one array value kind and one scope rule in
the value graph. The scope rule is the real cost: an array value has a length that belongs to an
instancer rather than to the node, so the compiler has to assign every array node to exactly one
instancing scope and refuse the graphs where that is ambiguous. That refusal is a new thing an
artist can run into, and its diagnostic has to say which two instancers claimed the node.

Editing one element re-evaluates the whole instancer in v0. The baseline measurement says the
alternative available today is no better, and it gives the later optimization -- per-element dirty
regions over a retained instancer image -- a number to beat: 18 ms at 500 elements, 61 ms at 2000.

The bounds-margin clip is a real behavioural difference from Merge, and an artist who parents an
instancer and drags it a long way will see clipped instances and a warning rather than pixels. The
alternative is an allocation with no upper bound, which is the defect the preflight estimate exists
to catch.

Primary references:

- [`instancing.md`](../architecture/instancing.md) -- the full model, the solver design, and the
  lane plan
- [`layer-graph-model.md`](../architecture/layer-graph-model.md) -- the value graph, drivers, socket
  kinds and the Layer Bounds read rule this record extends
- [`evaluation-primitives.md`](../architecture/evaluation-primitives.md) -- operation memoization,
  the memory ledger, and the semantics-version contract
- [ADR 0022](0022-language-neutral-host-boundary.md) -- the host boundary the scripting surface uses
