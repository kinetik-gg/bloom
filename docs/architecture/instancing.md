# Instancing And Per-Element Data

Status: proposed

Updated: 2026-09-17

## Purpose And Ownership

This document owns Bloom's instance model: one operation that produces an ordered set of N copies of
an upstream image with per-element attributes, where those attributes come from, and how layout and
constraints are expressed as pure value nodes over arrays of them.

[ADR 0023](../decisions/0023-instances-and-per-element-data.md) is the decision and the measured
evidence behind it. This document is the shape. It does not restate the value graph, drivers, socket
kinds or the operation cache -- [`layer-graph-model.md`](layer-graph-model.md) and
[`evaluation-primitives.md`](evaluation-primitives.md) own those, and instancing is deliberately
built out of them rather than beside them.

Nothing here is implemented. It is the contract INSTANCE-1 through INSTANCE-4 build against.

## The Instancer

`bloom.instancer` version 1 is an image operation in category `Compositing`, cardinality `Many`.

| Port | Kind | Meaning |
| --- | --- | --- |
| `prototype` (input) | Image | The picture each element is a copy of. An unconnected prototype is an empty image and the instancer draws nothing -- the same "half-built graph is not an error" rule a Layer Output already follows |
| `image` (output) | Image | All N elements composited in index order |

Its parameters are the instancer's OWN values -- the ones that describe the set rather than an
element of it:

| Role | Schema key | Type | Default | Animatable |
| --- | --- | --- | --- | --- |
| `count` | `bloom.instance.count` | Int64 | `1` | No |
| `seed` | `bloom.instance.seed` | Int64 | `0` | No |
| `blendMode` | `bloom.instance.blend-mode` | Int64 | `0` (`Normal`) | No |
| `origin` | `bloom.instance.origin` | Vec2d | composition centre at creation | Yes |

`count` is not animatable and not a curve, for the reason a blend mode is not: there is no
meaningful value between seven copies and eight, so a curve over it could only hold or jump. It can
still be DRIVEN by an Integer node, which is how a count varies at all, and a driven count makes the
instancer time-dependent exactly as a driven text content does.

`origin` is where element index 0 with no positional attribute lands. Every element attribute that
places geometry is an OFFSET from it, so an artist moves the whole set by moving one value and a
layout solver never has to know where the set lives.

An instancer creates no `LayerId`, no stack slot and no timeline row. It is a source of pixels that
happens to draw the same picture repeatedly; the Layer Output above it is still what makes those
pixels a layer.

## Per-Element Attributes

Each element carries the following. This is a CLOSED set in v0: an attribute that is not here cannot
reach the compositing stage, because every one of these has a defined meaning in the existing
resample and blend kernels and an arbitrary one would not.

| Attribute | Kind | Default | Meaning |
| --- | --- | --- | --- |
| `index` | Integer | `0..count-1` | Read-only. The element's position in the order |
| `count` | Integer | the instancer's count | Read-only. Present so an expression can normalize |
| `position` | Vector2 | `{0, 0}` | Offset from the instancer's `origin`, in its parent space |
| `rotation` | Scalar | `0` | Clockwise degrees about the element's own anchor |
| `scale` | Vector2 | `{1, 1}` | Per-axis factor about the element's own anchor |
| `opacity` | Scalar | `1` | Coverage multiplier in `[0, 1]` |
| `colour` | Color4 | opaque white | A straight multiply over the prototype's colour and alpha |
| `timeOffset` | Scalar | `0` | Seconds added to the prototype's evaluation time |
| `blendMode` | Integer | the instancer's mode | Only read through an explicit `Instance Merge` |

An element's anchor is the centre of the prototype's evaluated content bounds. There is no
per-element anchor attribute in v0: an anchor is a property of the picture being copied, and every
use an artist would put a per-element anchor to is expressible as a position offset.

`colour` is a MULTIPLY, not a material. It scales the prototype's premultiplied pixels, so tinting a
white prototype works and tinting a photograph darkens it, and neither of those needs a colour
pipeline stage. Per-instance materials are out of v0 (ADR 0023).

Named typed attributes -- arbitrary `Scalar`, `Vector2`, `Integer`, `Boolean` or `Color4` columns
carried alongside -- may be READ by solver nodes and routed into one of the rows above. They cannot
reach the compositing stage directly, and they cannot reach the prototype's own parameters at all;
that is per-instance evaluation, which is the eager expansion the design exists to avoid.

## Attribute Sources

Each attribute operand of the instancer is an ordinary parameter with an array-typed socket. Its
value therefore comes from exactly one of the two places a Bloom parameter's value has ever come
from -- its registered default, or its `DriverBinding` -- and the four "sources" below are simply
what can sit at the other end of that binding.

| Source | Shape | Notes |
| --- | --- | --- |
| Data block column | `Table Column` / `Point Set Positions` reading a DATA-1 block | The block's row count and the instancer's `count` disagree by clamping: extra rows are ignored, missing rows take the attribute's default, and a mismatch is one warning naming both numbers |
| Array expression | Any array value node over `Instance Index` and `Instance Count` | The general case; every other source is a convenience over it |
| Curve or ramp sample | `Sample Curve(curve, t[])` with `t = index / max(count - 1, 1)` | Reuses the existing exact rational sampler; a one-element set samples `t = 0` |
| Seeded random | `Instance Random(seed, minimum, maximum)` | See the seed policy below |

DATA-1 owns the block payloads and readers. Until it lands, the block column source is declared here
and unimplemented; nothing else in this document depends on it.

### Seed policy

`Instance Random` produces element `i`'s value from `hash(instancerSeed, i, attributeKey)` and
nothing else. Three properties follow, and all three are the point:

- **Adding an element never reshuffles the others.** The value at index 4 does not depend on the
  count, so raising a count from 20 to 21 adds a copy instead of rearranging the design.
- **Two attributes seeded alike do not correlate.** The attribute key is in the hash, so `rotation`
  and `opacity` on one seed are independent rather than the same sequence twice.
- **A cached or exported frame agrees with the frame that produced it.** There is no entropy source
  and no per-frame state, exactly as the existing `Random` value node has none. A seeded value
  varies over time only when something wires a changing number into the seed.

The hash is the versioned procedural hash from `src/core`; changing it changes pictures, so it is a
primitive-semantics move and not a free implementation detail.

## Array Values In The Value Graph

An instance set needs one number PER ELEMENT, and the value graph produces one number per frame. The
gap is closed by giving the value graph an array kind rather than by evaluating the graph N times.

### The kind

`CompiledValue` gains one appended alternative: an array of one of the existing kinds --
`Scalar[]`, `Vector2[]`, `Vector3[]`, `Integer[]`, `Boolean[]`, `Color4[]`. There is no array of
arrays and no array of `String`: the first is a shape no attribute has, and the second is a
per-element allocation with no consumer.

An array value's LENGTH is not authored and not part of the value. It is the count of the instancing
scope the node belongs to. That is the whole of why arrays need a scope rule.

### Instancing scope

Every array-valued node belongs to exactly ONE instancer: the instancer whose attribute operand the
node transitively feeds. The compiler assigns scopes by walking back from each instancer's attribute
operands before it lowers anything.

- A node reached from two instancers is refused with
  `bloom.runtime.compile.array-node-in-two-instancer-scopes`, naming the node and both instancers.
  Evaluating it twice would make one node mean two different lengths, and Bloom does not have a
  value whose type depends on who is reading it.
- A node reached from no instancer is dead and is not lowered, exactly as an unreachable image node
  is not lowered.
- A scope is not a subgraph the artist draws. It is derived, it has no record, and it never appears
  in the document.

### Broadcast and reduce

A SCALAR node feeding an array operand broadcasts: its one value is repeated for every element. The
compiler emits an explicit `CompiledValueBroadcast` operation for it rather than folding the
repetition into whoever reads the value, for the same reason every existing promotion compiles to
its own operation -- a widening that shows up in a plan dump is a widening that can be diagnosed.

An ARRAY feeding a scalar operand is REFUSED. Collapsing N values into one is a choice with several
answers, so it is a node an artist places: `Array Reduce(values[], selector)` with `Sum`, `Minimum`,
`Maximum` and `Mean`. Those four are in v0 because a spacing solver needs extents and a layout needs
totals; anything else waits for a use.

`Array Reduce` is the ONE way a value leaves an instancing scope, and its output is an ordinary
scalar in the ordinary value pass.

### Read rule

The array read rule is the analogue of the Layer Bounds read rule and is stated the same way:

> An array value may feed only the attribute operands of the instancer whose scope it belongs to,
> other array nodes in that same scope, or an `Array Reduce`. It may not drive a parameter of an
> image operation.

Compilation refuses the other destinations with
`bloom.runtime.compile.array-value-drives-image-operation`, naming the offending node and parameter.
The rule keeps the image plan acyclic and keeps "how many of these are there" answerable at compile
time.

One further refusal composes the two rules: an array node in an instancing scope may not be
downstream of a `Layer Bounds` readout. A bounds readout runs AFTER the image pass, and an instancer
is an image operation, so such a graph would ask the image pass for a value the image pass produces.
It is refused with `bloom.runtime.compile.bounds-readout-feeds-instancer` rather than being ordered
into a cycle.

### Evaluation order

```text
value pass (ordinary)        one number per node, as today
    |
    v
array pass (per instancer)   one array per node, in that instancer's scope, in topological order
    |
    v
image pass                   every image operation, instancers included
    |
    v
post-image value pass        Layer Bounds readouts and their dependants, as today
```

The array pass is a third pass, not an interleaving. It runs after the ordinary value pass because
array nodes read ordinary values (a `count`, a spacing, a radius), and before the image pass because
an instancer reads arrays. Each scope is one linear sweep in topological order, exactly like the
existing value pass, with no re-entrancy.

## Layout And Constraint Solvers

Layout is not a feature of the instancer. It is a set of pure array value nodes that produce a
`position[]` and sometimes a `rotation[]`, which an artist wires into the instancer's attributes --
or does not, and places elements some other way.

| Node | Inputs | Outputs | Behaviour |
| --- | --- | --- | --- |
| `Instance Index` | none | `index` Integer[] | `0..count-1` for its scope |
| `Instance Count` | none | `count` Integer | The scope's count, as an ordinary scalar |
| `Grid Layout` | `columns` Integer (1), `spacing` Vector2, `centre` Boolean; selector `order` (Row major) | `position` Vector2[] | Row-major or column-major; `centre` shifts the whole block so its bounding box is centred on the origin |
| `Radial Layout` | `radius` Scalar, `startAngle` Scalar, `sweep` Scalar (360), `alignToTangent` Boolean | `position` Vector2[], `rotation` Scalar[] | A full 360 sweep does not duplicate the endpoint; a partial sweep includes both ends |
| `Path Follow` | `path` Path, `offset` Scalar, `alignToTangent` Boolean; selector `spacing` (Even) | `position` Vector2[], `rotation` Scalar[] | Even arc-length spacing, or fixed spacing from the start. Arc length is measured on the existing bounded cubic flattening, so a layout and the stroke of the same path agree |
| `Spacing Relax` | `position` Vector2[], `radius` Scalar[], `strength` Scalar (1); selector `iterations` (8) | `position` Vector2[] | Fixed-count pairwise separation; see determinism below |
| `Sample Curve` | `curve` reference, `t` Scalar[] | `value` Scalar[] | The existing exact rational sampler, once per element |
| `Instance Random` | `seed` Integer, `minimum`, `maximum` Scalar | `value` Scalar[] | The seed policy above |
| `Array Reduce` | `values` Scalar[]; selector `reduction` (Sum) | `result` Scalar | Sum, Minimum, Maximum, Mean |

`Repeater` and `Scatter` from [`node-catalogue.md`](../product/node-catalogue.md) are the artist's
names for an instancer pre-wired to `Grid Layout` and to `Instance Random`. They are Add-surface
presets that build a small graph in one undoable transaction, exactly as `AddSolidLayer` builds a
topology, and not separate operations with their own math.

### Determinism

Every solver is a pure function of its operands. That is easy for a grid and is a real constraint
for relaxation, so it is spelled out:

- **Iteration count is a compile-time selector**, capped at 64. It is not a convergence threshold.
  An early exit on "close enough" makes the result depend on floating-point noise, and two machines
  that stopped on different iterations would render different pictures from one document.
- **Neighbours are enumerated in index order**, and each iteration reads the previous iteration's
  positions rather than the one being written. A Gauss-Seidel sweep would make the answer depend on
  traversal order, which is an implementation detail that must not be a picture.
- **All arithmetic goes through the frozen scalar primitives**, so a solver inherits their
  signed-zero, subnormal, rounding-environment and failure rules rather than growing its own.
- **No wall clock, no frame counter, no accumulated state.** A solver at frame 100 does not depend on
  frame 99. Temporal feedback needs the explicit delay node the layer/graph model already reserves.

### Diagnostics and fallbacks

Solvers follow the value library's fallback philosophy: a failure substitutes a documented value and
records a scoped diagnostic, because a frame that renders wrong-but-visible is more useful to an
artist than a frame that does not render.

| Failure | Fallback |
| --- | --- |
| A layout operand outside its domain (zero columns, negative radius) | The identity layout: every element at the origin, reported |
| An empty or degenerate path in `Path Follow` | The identity layout, reported |
| A block column shorter than the count | The attribute's default for the missing elements, reported once with both counts |
| A block column of the wrong kind | The attribute's default for every element, reported; `bloom.runtime.compile.instance-attribute-kind-mismatch` when the kind is knowable at compile time |
| `Array Reduce` over an empty array | `0`; an empty set has no sum an artist could act on |

## Compiled Plan Shape

```text
CompiledInstancer {
    document::NodeId sourceNodeId;
    OperationIndex prototype;
    CompiledScalarParameter origin;          // Vec2 operand in practice
    std::int64_t count;                      // or a ValueOutputIndex beside it when driven
    std::int64_t seed;
    core::BlendMode blendMode;
    std::array<CompiledInstanceAttribute, kInstanceAttributeCount> attributes;
}

CompiledInstanceAttribute {
    document::ParameterId id;
    std::variant<CompiledValue, ValueOutputIndex> source;   // constant default, or an array output
}
```

`CompiledInstancer` is APPENDED to the `CompiledOperation` variant and the array alternative is
APPENDED to `CompiledValue`, so every existing alternative index -- which the operation key encodes
-- keeps its meaning. The count sits beside an optional driver exactly as
`CompiledLayerOutput::blendMode` sits beside `drivenBlendMode`, and for the same reason: a kind that
cannot interpolate has no curve alternative to gain.

Array value operations are ordinary `CompiledValueOperation` entries carrying a new
`instancerScope` index and running in the array pass. The flat value-output table is shared: an
array output claims one entry holding an array value, not N entries.

## Evaluation

The instancer's stage, in order:

1. Read the resolved attribute arrays out of the array pass. Their lengths are already the count;
   a length that is not is an invalid plan, not a per-frame clamp.
2. Compute the exact content bounds as the union of the N transformed prototype rectangles. This is
   rectangle arithmetic over the prototype's evaluated bounds -- N rectangles, not N images -- so
   bounds cost is independent of resolution.
3. Intersect the union with the storage window (composition display window grown by one composition
   extent on each axis) to get the data window, and warn if the intersection dropped content.
4. Allocate the output once, and ONE per-element scratch buffer sized to the largest transformed
   element. The scratch is reused for every element; this is what keeps the resident set at three
   frames rather than N.
5. For each element in REVERSE index order -- index 0 is topmost, so the fold visits the bottom
   first, exactly as the Layer Stack does -- resample the prototype into the scratch through
   `layerTransformBilinearRow()` with that element's matrix, apply `opacity` and `colour`, and
   composite it into the output through `blendLinearRec709SceneRow()` over only the rows and columns
   that element occupies.
6. Publish the output, its content bounds, and the per-element transforms as derived state beside
   `EvaluatedOperationBounds`.

Cancellation is checked at element and scanline boundaries. Progress reports `count` units. An
element whose transformed bounds are empty -- a zero scale, or a position entirely outside the
storage window -- publishes nothing and is skipped, which is what compositing an empty copy means.

A prototype that is a vector chain keeps its vector provenance per element: the instancer composes
each element's matrix with the chain's matrix and rasterises the geometry at output resolution,
exactly as a Layer Output does, rather than resampling a raster. A raster prototype resamples once
per element.

### Preflight

`CompiledInstancer` contributes `prototype + scratch + output` to the preflight resident estimate,
never `count` frames. This is the measured defect the design exists to repair: the expanded baseline
estimates 15.5 GiB at 500 elements against 97 MiB actually used
([ADR 0023](../decisions/0023-instances-and-per-element-data.md), "Measured baseline").

## Caching And Memory

The instancer is one operation and takes one operation-cache entry. Its content key is the existing
preamble -- project, composition, format, resolution, ROI, time when time-dependent, operation kind,
source node -- plus:

- the prototype's content hash, through the existing `forEachInput` fold;
- the resolved `origin`, `count`, `seed` and blend mode; and
- an **instance-set digest**: a SHA-256 over every resolved attribute array, encoded with the
  existing `OperationKey` byte rules, so floating-point bits including signed zero participate
  without rounding or locale-dependent text.

Two consequences, both deliberate:

- **An unchanged instance set is one lookup.** The spike measured the equivalent for the expanded
  baseline: 503 hits, zero misses, 46x faster than the cold frame.
- **Editing one element invalidates the whole instancer image.** Content addressing cannot give
  per-element granularity to an operation that is one entry. The baseline measurement says the
  alternative available today is no better -- re-folding a Merge after a one-element edit costs
  18 ms at 500 elements and 61 ms at 2000 -- and it gives the later per-element dirty-region
  optimization a number to beat.

No new memory pool. Instancer images are ordinary entries against `playback/operation-cache-bytes`
from the session's one `MemoryBudgetLedger`, and the scratch buffer is transient storage charged to
the request's `pixelStorageByteLimit`. Attribute arrays are retained with the cache entry and
counted in its byte size, capped at 4 MiB per instancer.

## Time

`timeOffset` changes the time at which the PROTOTYPE is evaluated for one element. A prototype
evaluated at several times is several evaluations, which is exactly the cost the design otherwise
refuses to pay, so v0 bounds it:

- A non-zero `timeOffset` is supported only when the prototype is a `CompiledCompositionSource`.
  Nested plans already evaluate at a mapped time, already carry their own plan, and already key
  their cache by mapped time and nested revision.
- Any other prototype with a non-zero offset is refused at compile with
  `bloom.runtime.compile.instance-time-offset-unsupported-prototype`, naming the prototype's node.
  Silently ignoring the offset would render a picture the document did not ask for.
- Offsets are deduplicated by MAPPED TIME, so a hundred elements sharing five distinct offsets cost
  five nested evaluations. The instancer's cache key carries the sorted distinct mapped times.
- A non-constant `timeOffset` attribute, a driven `count`, and a time-dependent prototype each make
  the instancer time-dependent under the existing transitive analysis.

The mapping itself is the one in [`animation-and-time.md`](animation-and-time.md), "Composition
Source Time Mapping". Instancing adds an offset to the composition time; it does not invent a second
time model.

## Selection, Gizmo And Properties

- **Selecting an instance selects the instancer.** An element has no `LayerId`, no `NodeId` and no
  parameter identity, so there is nothing else a selection could be. Picking in the viewer hit-tests
  the union of element polygons and resolves to the instancer node.
- **The viewer draws the instancer's own gizmo** -- the transform of the Layer Output above it, as
  for any other source -- plus read-only ghost outlines for up to 200 elements, with a "showing 200
  of N" readout when the set is larger. Ghosts are not handles: dragging one drags the instancer.
- **Properties shows the instancer's own values and one row per attribute binding.** A bound
  attribute shows the driver node's name behind a link glyph and the resolved value for element 0 at
  the session time, read-only, exactly as the existing driven-parameter row does. An unbound
  attribute shows its default as an ordinary inline widget.
- **The timeline shows one row**, and its twirl-down carries the upstream value nodes reached
  through driver links, by the rule in [`animation-and-time.md`](animation-and-time.md), "Upstream
  Value Nodes In The Timeline". An array node's own animatable literals are keyable there; there are
  no per-element keys, because there are no per-element parameters.

## Scripting Surface

Instancer authoring is typed commands in the existing registry, so ADR 0022's facade, `bloom-cli`,
Python and MCP receive it with no bespoke path:

| Operation | Effect |
| --- | --- |
| `bloom.instancer.add` | Creates an instancer, optionally with a prototype connection, in one transaction |
| `bloom.instancer.set-count` | Sets the count, refusing a non-positive value or one past the cap |
| `bloom.instancer.bind-attribute` | Points an attribute operand at an array value output -- an ordinary `ConnectPorts` into an operand socket, which is a driver binding |
| `bloom.instancer.clear-attribute` | Restores the attribute's registered default, an ordinary `DisconnectInput` |
| `bloom.layout.add-preset` | Builds a Repeater or Scatter graph in one undoable transaction |

Reads follow the same split the rest of the facade follows: the declared bindings, the count and the
seed are DOCUMENT data on an immutable snapshot; the resolved per-element arrays are DERIVED state,
available only as a query against an evaluated frame, in the same place evaluated bounds are. No
client code executes per element, and none executes inside the evaluator.

## Identity And Semantics Versions

INSTANCE-1 is designed to move no semantics version and re-derive no golden. The conditions are
exact, and any lane that breaks one owns the re-derivation:

1. `CompiledInstancer` is appended LAST to the `CompiledOperation` variant, and the array
   alternative is appended LAST to `CompiledValue`. The operation key encodes `variant::index()`, so
   appending keeps every existing plan value meaning what it meant.
2. No existing compiled field changes its meaning. The new operation carries its own fields.
3. The composite reuses `layerTransformBilinearRow()` and `blendLinearRec709SceneRow()` unchanged.
   A new kernel, or a change to either of these, is an image-primitive move.

Under those conditions the current identities -- plan 7, animation sampling 2, evaluator 8, image
primitives 7 -- stand, and no cached or exported frame digest shifts. If a condition is broken, the
lane reproduces every current pin through `s5-identity-oracle.py` BEFORE deriving replacements, the
discipline every previous identity move has followed.

The document schema floor stays 1.16. A new node type stores only its type id, and an attribute
binding is the existing `DriverBinding` record. Array operands carry no authored constant, which is
what keeps a stored array out of the format.

## Diagnostics

| Id | Raised when |
| --- | --- |
| `bloom.runtime.compile.instance-count-exceeds-limit` | A resolved count is above 10,000, or nested instancer counts multiply past it |
| `bloom.runtime.compile.instance-attribute-budget-exceeded` | Attribute storage for one instancer exceeds 4 MiB, or more than 32 attributes are bound |
| `bloom.runtime.compile.array-node-in-two-instancer-scopes` | One array node is reached from two instancers |
| `bloom.runtime.compile.array-value-drives-image-operation` | An array value reaches an image operation's parameter |
| `bloom.runtime.compile.bounds-readout-feeds-instancer` | An array node in an instancing scope is downstream of a Layer Bounds readout |
| `bloom.runtime.compile.instance-time-offset-unsupported-prototype` | A non-zero time offset on a prototype that is not a Composition Source |
| `bloom.runtime.compile.instance-attribute-kind-mismatch` | A bound source's kind is not the attribute's kind and no promotion admits it |

Evaluation-time failures are warnings beside a rendered frame: bounds clipped at the storage margin,
a block column shorter than the count, and each solver fallback above.

## Implementation Plan

Four lanes. Each is a merge-gated slice with its own fence; none of them lands product code that a
later one has to rewrite.

### INSTANCE-1 -- model, compiler, evaluator

Scope: `bloom.instancer` node definition and registration; `CompiledInstancer` and
`CompiledInstanceAttribute`; snapshot lowering; the evaluator stage, its bounds, preflight
contribution, cancellation and progress; the instance-set digest in the operation key; the count,
attribute and storage caps with their diagnostics. Attributes come only from their defaults and from
ordinary scalar broadcast in this slice -- no arrays yet, so an instancer draws N copies in one place
until INSTANCE-2 gives it a layout. That is deliberately a boring picture and a complete vertical.

Fence: `src/document/node_definition_registry.*`, `src/runtime/compiled_plan.hpp`,
`snapshot_compiler*`, `cpu_composition_evaluator.cpp` and its tests.

Gates: evaluator tests pinning one, two and 500 elements against independently stated pixel
expectations; a bounds test pinning the union and the storage clip; a cache test pinning one entry
and a digest hit; a preflight test pinning three frames rather than N; the identity goldens
UNCHANGED, which is the slice's real assertion.

### INSTANCE-2 -- attribute sources and solvers

Scope: the array value kind; the instancing-scope assignment pass and its two refusals; the array
pass in the evaluator; `CompiledValueBroadcast` and `Array Reduce`; `Instance Index`,
`Instance Count`, `Instance Random`, `Sample Curve`; `Grid Layout`, `Radial Layout`, `Path Follow`,
`Spacing Relax`. Block columns land here if DATA-1 has landed, and are held back as one follow-up
otherwise.

Fence: `src/document/value_*`, `src/runtime/value_graph_evaluation.cpp`,
`snapshot_compiler_value_graph.ipp`, and the value node library document.

Gates: determinism tests running each solver twice in one process and once in a second process and
comparing bit for bit; a relaxation test pinning the fixed iteration count and index-order
traversal; refusal tests for both scope violations and the bounds-readout composition; a seed-policy
test proving that raising the count leaves existing elements' values unchanged.

### INSTANCE-3 -- UI

Scope: the instancer node card with its attribute rows; Properties rows for bindings, driven and
undriven; the viewer ghost overlay with its 200-element cap and readout; hit-testing that resolves an
element to the instancer; the Add-surface Repeater and Scatter presets; timeline projection of the
upstream value nodes.

Fence: `src/ui` only. No document, plan or evaluator change.

Gates: whole-window goldens at both device pixel ratios; the UI grammar allowlist stays at zero; an
interaction test proving a drag on a ghost moves the instancer and never an element; a Properties
test proving a bound attribute shows the driver name and a read-only resolved value.

A UI-grammar addition is likely -- a `Layout` family for the solver nodes in the Add surface's
category order -- and it belongs to the UI grammar's owner rather than to this lane. INSTANCE-3
proposes it and does not land it unilaterally.

### INSTANCE-4 -- scripting

Scope: the five registry operations above, their argument schemas and typed refusals; the facade
queries for declared bindings and resolved count; the derived-state query for resolved arrays on an
evaluated frame; `bloom-cli` coverage.

Fence: `src/scripting`, `src/commands`.

Gates: a registry test proving every new `Operation::typeId()` is discoverable with an argument
schema; a headless test building a 500-element instancer and rendering a frame through `bloom-cli`;
a capability test proving the resolved-array query refuses a request with no evaluated frame rather
than evaluating one as a side effect.

### Sequencing, and what may run in parallel

INSTANCE-1 must land first: everything else binds to `CompiledInstancer`. INSTANCE-3 and INSTANCE-4
may run in parallel with each other once INSTANCE-2's node set is frozen, because their fences do not
overlap. INSTANCE-2 depends on INSTANCE-1 only for the attribute operands, so its value-graph work
can start against the frozen operand list before the evaluator stage is complete.

### Risks

| Risk | Mitigation |
| --- | --- |
| The scope rule refuses graphs artists expect to work | The refusal names both instancers and the node. INSTANCE-2's gate includes a fixture for the shape artists will actually draw -- one layout feeding two instancers -- so the message is written against a real mistake rather than a hypothetical one |
| A one-element edit costs a whole re-composite | Measured and accepted for v0 (18 ms at 500, 61 ms at 2000). Per-element dirty regions are a named post-v0 lane with that number as its target |
| Vector provenance per element multiplies rasterisation cost | INSTANCE-1's gate measures a vector prototype at 500 elements beside the raster one. If the vector path is not within a documented factor, the instancer rasterises the prototype once and resamples, with a diagnostic explaining the fallback |
| The storage-margin clip surprises an artist who parents an instancer | The clip is a warning with the instancer named, not a silent crop, and the margin is a whole composition extent on each axis |
| DATA-1 lands later than INSTANCE-2 | The block column source is one of four and the only one that depends on DATA-1. INSTANCE-2 ships the other three and a stub definition for the fourth |
| The count cap is wrong | It is a constant with a diagnostic, not a shape. Raising it is a one-line change once a real project says 10,000 is too few, and lowering it is not, which is why it starts where a measured frame is already too slow |
